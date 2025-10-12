/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qcom_qcc730_dxe

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <string.h>

#include "soc.h"

LOG_MODULE_REGISTER(dxe_qcc730, CONFIG_DMA_LOG_LEVEL);

/* Definitions of bitfields in the DXE descriptor control word */
#define QCC730_DXE_DESC_CTRL_VALID      0x00000001
#define QCC730_DXE_DESC_CTRL_XTYPE_MASK 0x00000006
#define QCC730_DXE_DESC_CTRL_XTYPE_H2H  0x00000000
#define QCC730_DXE_DESC_CTRL_XTYPE_B2B  0x00000002
#define QCC730_DXE_DESC_CTRL_XTYPE_H2B  0x00000004
#define QCC730_DXE_DESC_CTRL_XTYPE_B2H  0x00000006
#define QCC730_DXE_DESC_CTRL_EOP        0x00000008
#define QCC730_DXE_DESC_CTRL_BDH        0x00000010
#define QCC730_DXE_DESC_CTRL_SIQ        0x00000020
#define QCC730_DXE_DESC_CTRL_DIQ        0x00000040
#define QCC730_DXE_DESC_CTRL_PIQ        0x00000080
#define QCC730_DXE_DESC_CTRL_PDU_REL    0x00000100
#define QCC730_DXE_DESC_CTRL_BTHLD_SEL  0x00001e00
#define QCC730_DXE_DESC_CTRL_PRIO       0x0000e000
#define QCC730_DXE_DESC_CTRL_STOP       0x00010000
#define QCC730_DXE_DESC_CTRL_INT        0x00020000
#define QCC730_DXE_DESC_CTRL_BDT_IDX    0x000c0000
#define QCC730_DXE_DESC_CTRL_BDT_SWAP   0x00100000
#define QCC730_DXE_DESC_CTRL_ENDIANNESS 0x00200000
#define QCC730_DXE_DESC_CTRL_RSVD       0xffc00000

/* Position of the channel priority in the DXE descriptor control word */
#define QCC730_DXE_DESC_CTRL_PRIO_Pos 13U

#define QCC730_DEFAULT_RRAM_WRITE_DLY 2U

/* Offset between two groups of DXE channel registers */
#define DXE_CH_REGS_OFFSET                                                                         \
	(offsetof(DXE_0_BASE_dxe_0_Type, DXE_0_CH1_CTRL) -                                         \
	 offsetof(DXE_0_BASE_dxe_0_Type, DXE_0_CH0_CTRL))

/* Helper macro to access DXE channel registers */
#define DXE_CH_REG(base, ch, reg)                                                                  \
	((__typeof__(&(base)->DXE_0_CH0_##reg))((uint8_t *)&(base)->DXE_0_CH0_##reg +              \
						(size_t)(ch) * DXE_CH_REGS_OFFSET))

/* Converts 4-bit Zephyr priority to 3-bit QCC730 channel priority */
#define TO_3BIT_PRIO(x) ((x) >> 1U)

/* Arbitrary configuration for maximum number of transfer blocks */
#define QCC730_DXE_MAX_BLOCKS 16

/* Maximum transfer size aligned to 4 bytes */
#define QCC730_DXE_MAX_BLOCK_SIZE 0x3FFC

struct qcc730_dxe_desc_short {
	uint32_t ctrl;
	uint32_t xfr_size;
	uint32_t src_addr_l;
	uint32_t dst_addr_l;
	uint32_t next_desc_addr_l;
} __aligned(16);

struct qcc730_dxe_config {
	DXE_0_BASE_dxe_0_Type *dxe;
	CCU_BASE_ccu_Type *ccu;
	uint8_t num_channels;
	void (*irq_configure)(void);
};

struct qcc730_dxe_data {
	dma_callback_t callback;
	void *user_data;
	struct qcc730_dxe_desc_short desc[QCC730_DXE_MAX_BLOCKS];
	struct k_spinlock lock;
};

static void qcc730_dxe_isr(const struct device *dev)
{
	const struct qcc730_dxe_config *cfg = dev->config;

	for (uint32_t ch = 0U; ch < cfg->num_channels; ch++) {
		int status = -EIO;
		struct qcc730_dxe_data *ch_data = &((struct qcc730_dxe_data *)dev->data)[ch];

		if (IS_BIT_SET(cfg->dxe->DXE_0_INT_SRC_RAW.reg, ch)) {
			if (IS_BIT_SET(cfg->dxe->DXE_0_INT_DONE_SRC.reg, ch)) {
				status = 0;
			}
			cfg->dxe->DXE_0_INT_CLR.reg = BIT(ch);

			if (ch_data->callback) {
				ch_data->callback(dev, ch_data->user_data, ch, status);
			}
		}
	}
}

static int qcc730_dxe_configure(const struct device *dev, uint32_t channel,
				struct dma_config *config)
{
	const struct qcc730_dxe_config *cfg = dev->config;
	struct qcc730_dxe_data *data = &((struct qcc730_dxe_data *)dev->data)[channel];
	struct qcc730_dxe_desc_short *desc = data->desc;
	struct dma_block_config *block = NULL;

	if (channel >= cfg->num_channels) {
		LOG_ERR("Unsupported DXE channel %d - must be < %d", channel, cfg->num_channels);
		return -EINVAL;
	}

	if (!config || config->block_count == 0 || config->block_count > QCC730_DXE_MAX_BLOCKS ||
	    !config->head_block) {
		LOG_ERR("Wrong DXE config");
		return -EINVAL;
	}

	if (config->channel_direction != MEMORY_TO_MEMORY) {
		LOG_ERR("DXE supports only MEMORY_TO_MEMORY transfers");
		return -EINVAL;
	}

	if (DXE_CH_REG(cfg->dxe, channel, STATUS)->bit.BUSY) {
		LOG_ERR("DXE channel %u busy", channel);
		return -EBUSY;
	}

	k_spinlock_key_t key = k_spin_lock(&data->lock);

	uint32_t prio3 = TO_3BIT_PRIO(config->channel_priority) & 0x7;

	/* Prepare control word for DXE descriptor */
	uint32_t ctrl = QCC730_DXE_DESC_CTRL_VALID | QCC730_DXE_DESC_CTRL_XTYPE_H2H |
			(prio3 << QCC730_DXE_DESC_CTRL_PRIO_Pos) | QCC730_DXE_DESC_CTRL_ENDIANNESS;

	/* Fill in the descriptors array for the channel */
	for (block = config->head_block; block != NULL; block = block->next_block) {
		if (block->block_size == 0 || block->block_size > QCC730_DXE_MAX_BLOCK_SIZE) {
			LOG_ERR("Invalid block size %u", block->block_size);
			k_spin_unlock(&data->lock, key);
			return -EINVAL;
		}

		if (!IS_ALIGNED(block->source_address, 4U) ||
		    !IS_ALIGNED(block->dest_address, 4U)) {
			LOG_ERR("DXE source/dest addresses must be 4-byte aligned");
			k_spin_unlock(&data->lock, key);
			return -EINVAL;
		}

		desc->ctrl = ctrl;
		desc->xfr_size = block->block_size;
		desc->src_addr_l = block->source_address;
		desc->dst_addr_l = block->dest_address;
		if (block->next_block != NULL) {
			desc->next_desc_addr_l = (uint32_t)(desc + 1);
		} else {
			/* Last descriptor - enable the interrupt */
			desc->ctrl |= QCC730_DXE_DESC_CTRL_INT;
			desc->next_desc_addr_l = (uint32_t)desc;
		}
		desc++;
	}

	/* Set transfer type - only MEMORY_TO_MEMORY supported */
	DXE_CH_REG(cfg->dxe, channel, CTRL)->bit.XTYPE = 0U; /* MMORY_TO_MEMORY is H2H */
	/* Set priority - Zephyr uses 4 bits, and QCC730 uses 3 bits*/
	DXE_CH_REG(cfg->dxe, channel, CTRL)->bit.PRIO = TO_3BIT_PRIO(config->channel_priority);
	DXE_CH_REG(cfg->dxe, channel, CTRL)->bit.ENDIANNESS = 1U; /* little endian */
	DXE_CH_REG(cfg->dxe, channel, CTRL)->bit.EDVEN = 1U;
	DXE_CH_REG(cfg->dxe, channel, CTRL)->bit.EDEN = 1U;
	/* Enable channel interrupts */
	DXE_CH_REG(cfg->dxe, channel, CTRL)->bit.INE_DONE = 1U;
	DXE_CH_REG(cfg->dxe, channel, CTRL)->bit.INE_ERR = 1U;
	DXE_CH_REG(cfg->dxe, channel, CTRL)->bit.INE_ED = 1U;
	/* Use short descriptors */
	DXE_CH_REG(cfg->dxe, channel, CTRL)->bit.DFMT = 0U;

	data->callback = config->dma_callback;
	data->user_data = config->user_data;

	LOG_DBG("Configured DXE channel %d", channel);

	k_spin_unlock(&data->lock, key);

	return 0;
}

static int qcc730_dxe_start(const struct device *dev, uint32_t channel)
{
	const struct qcc730_dxe_config *cfg = dev->config;
	struct qcc730_dxe_data *data = &((struct qcc730_dxe_data *)dev->data)[channel];

	if (channel >= cfg->num_channels) {
		return -EINVAL;
	}

	if (DXE_CH_REG(cfg->dxe, channel, STATUS)->bit.BUSY) {
		LOG_ERR("DXE channel %u busy", channel);
		return -EBUSY;
	}

	LOG_DBG("Starting DXE channel %d", channel);

	k_spinlock_key_t key = k_spin_lock(&data->lock);

	/* Clear any pending interrupts on this channel before enabling */
	cfg->dxe->DXE_0_INT_CLR.reg = BIT(channel);

	/* Setup the descriptor list address to first item in descriptors' array */
	DXE_CH_REG(cfg->dxe, channel, DESCH)->reg = 0U;
	DXE_CH_REG(cfg->dxe, channel, DESCL)->reg = (uint32_t)&data->desc[0];

	/* Enable the channel */
	DXE_CH_REG(cfg->dxe, channel, CTRL)->bit.EN = 1U;
	cfg->dxe->DXE_0_DMA_ENCH.reg |= BIT(channel);

	k_spin_unlock(&data->lock, key);

	return 0;
}

static int qcc730_dxe_stop(const struct device *dev, uint32_t channel)
{
	const struct qcc730_dxe_config *cfg = dev->config;

	if (channel >= cfg->num_channels) {
		return -EINVAL;
	}

	/* Abort transaction */
	DXE_CH_REG(cfg->dxe, channel, CTRL)->bit.ABORT = 1U;
	/* Disable the channel */
	DXE_CH_REG(cfg->dxe, channel, CTRL)->bit.EN = 0U;
	cfg->dxe->DXE_0_DMA_ENCH.reg &= ~BIT(channel);

	LOG_DBG("Stopped DXE channel %d", channel);
	return 0;
}

static int qcc730_dxe_get_status(const struct device *dev, uint32_t channel,
				 struct dma_status *stat)
{
	const struct qcc730_dxe_config *cfg = dev->config;

	if (channel >= cfg->num_channels || stat == NULL) {
		return -EINVAL;
	}

	stat->busy = (DXE_CH_REG(cfg->dxe, channel, STATUS)->bit.BUSY == 1U);
	stat->pending_length = DXE_CH_REG(cfg->dxe, channel, SZ)->bit.REM_SZ;
	stat->dir = MEMORY_TO_MEMORY; /* only MEMORY_TO_MEMORY supported */

	return 0;
}

static const struct dma_driver_api qcc730_dma_api = {
	.config = qcc730_dxe_configure,
	.start = qcc730_dxe_start,
	.stop = qcc730_dxe_stop,
	.get_status = qcc730_dxe_get_status,
};

static int qcc730_dxe_init(const struct device *dev)
{
	const struct qcc730_dxe_config *config = dev->config;

	/* Softreset the DXE via CCU */
	config->ccu->CCU_R_CCU_SOFT_RESET.bit.DXE_SOFT_RESET = 1U;
	config->ccu->CCU_R_CCU_SOFT_RESET.bit.DXE_SOFT_RESET = 0U;

	/* Clear DXE config */
	config->dxe->DXE_0_DMA_CSR.reg = 0U;

	/* Apply config to DXE */
	config->dxe->DXE_0_DMA_CSR.bit.EN = 1U;
	config->dxe->DXE_0_DMA_CSR.bit.ECTR_EN = 1U;
	config->dxe->DXE_0_DMA_CSR.bit.H2H_SYNC_EN = 1U;
	config->dxe->DXE_0_DMA_CSR.bit.TSTMP_EN = 1U;
	config->dxe->DXE_0_DMA_CSR.bit.RRAM_WRITE_DLY = QCC730_DEFAULT_RRAM_WRITE_DLY;

	/* Clear channel counters */
	config->dxe->DXE_0_CTR_CLR.reg = BIT_MASK(config->num_channels);

	/* Connect IRQ(s) */
	config->irq_configure();

	return 0;
}

#define QCC730_DXE_IRQ_CONNECT(n, inst)                                                            \
	IRQ_CONNECT(DT_INST_IRQ_BY_IDX(inst, n, irq), DT_INST_IRQ_BY_IDX(inst, n, priority),       \
		    qcc730_dxe_isr, DEVICE_DT_INST_GET(inst), 0);                                  \
	irq_enable(DT_INST_IRQ_BY_IDX(inst, n, irq));

#define CONFIGURE_ALL_IRQS(inst, n) LISTIFY(n, QCC730_DXE_IRQ_CONNECT, (), inst)

#define QCC730_INST_INIT(inst)                                                                     \
	static struct qcc730_dxe_data qcc730_dxe_##inst##_data[DT_INST_PROP(inst, dma_channels)];  \
	static void qcc730_dxe##inst##_irq_configure(void)                                         \
	{                                                                                          \
		CONFIGURE_ALL_IRQS(inst, DT_NUM_IRQS(DT_DRV_INST(inst)));                          \
	}                                                                                          \
	static const struct qcc730_dxe_config qcc730_dxe_cfg_##inst = {                            \
		.dxe = (DXE_0_BASE_dxe_0_Type *)DT_INST_REG_ADDR_BY_NAME(inst, dxe),               \
		.ccu = (CCU_BASE_ccu_Type *)DT_INST_REG_ADDR_BY_NAME(inst, ccu),                   \
		.num_channels = DT_INST_PROP(inst, dma_channels),                                  \
		.irq_configure = qcc730_dxe##inst##_irq_configure,                                 \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, qcc730_dxe_init, NULL, &qcc730_dxe_##inst##_data,              \
			      &qcc730_dxe_cfg_##inst, PRE_KERNEL_1, CONFIG_DMA_INIT_PRIORITY,      \
			      &qcc730_dma_api);

DT_INST_FOREACH_STATUS_OKAY(QCC730_INST_INIT)
