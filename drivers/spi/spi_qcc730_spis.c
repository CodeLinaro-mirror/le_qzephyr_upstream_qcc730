/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT qcom_qcc730_spi

#include <zephyr/device.h>
#include <soc.h>
#include <zephyr/pm/device.h>

#define LOG_LEVEL CONFIG_SPI_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(spi_qcc730_spis);

#include <spi_context.h>

#define QCC730_QCSPI_SLAVE_FR_BOOT_STRAP_VALUE                                                             \
	0x63887466 /* Magic number to unlock BOOT_STRAP_CONFIGURATION register */
#define QCC730_QCSPI_AHB_PRI_CONFIG 0xF

/*
Agreed config for the QcSPI:
Please look at register bitfield definitons for more details, for the register QCSPI_SLAVE_CONFIG
QCSPI_CONFIG_ACC_SIZE_WORD is 0 (4 bytes)
QCSPI_CONFIG_N_DUMMY is 10 bytes
QCSPI_CONFIG_HOST_CTRL is 0
QCSPI_CONFIG_ADDR_BYTE_LEN is 4 bytes
QCSPI_CONFIG_RDBREN is unset
QCSPI_CONFIG_WRBREN is unset
QCSPI_CONFIG_WPDIS is set
QCSPI_CONFIG_SEQMOD is 0, little endian
QCSPI_CONFIG_CPOL is 0
QCSPI_CONFIG_CPHA(0) is 0
QCSPI_CONFIG_CORE_DIS is unset
QCSPI_CONFIG_EXT_BASE_ADDR_LOCK(1) is set
QCSPI_CONFIG_SPI_ACC_CTRL is unset
*/
#define QCC730_QCSPI_HOST_CONFIG 0x0a050800

#define QCSPI_SLAVE_HOST_INT0_MASK      0x1000000
#define QCSPI_SLAVE_HOST_INT1_MASK      0x2000000
#define QCSPI_SLAVE_HOST_INT2_MASK      0x4000000

#define QCSPI_SLAVE_ENABLE              0x1
#define QCSPI_SLAVE_DISABLE             0x0

const struct device *gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpioa));

struct spi_qcc730_data {
	struct spi_context ctx;
};

struct spi_qcc730_cfg {
	QCSPI_SLAVE_BASE_qcspi_slave_Type *regs;
	PMU_BASE_pmu_Type *pmu;
	uint8_t pin_num;
};

static DWSPI_SLAVE_BASE_dwspi_slave_Type *dwspi_regs = (DWSPI_SLAVE_BASE_dwspi_slave_Type *)QCC730V2_DWSPI_SLAVE_BASE;
static CDAHB_BASE_cdahb_Type *cdahb_regs = (CDAHB_BASE_cdahb_Type *)0x011a0000; // QWLAN_CDAHB_BASE

static int spi_qcc730_spis_transceive(const struct device *dev, const struct spi_config *spi_cfg,
				      const struct spi_buf_set *tx_bufs,
				      const struct spi_buf_set *rx_bufs)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(spi_cfg);
	ARG_UNUSED(tx_bufs);
	ARG_UNUSED(rx_bufs);

	LOG_ERR("Transceive is not supported by qcspi");

	return -ENOTSUP;
}

static int spi_qcc730_spis_release(const struct device *dev, const struct spi_config *spi_cfg)
{
	struct spi_qcc730_data *data = dev->data;

	ARG_UNUSED(spi_cfg);

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

/* Forward declaration for ring service handler */
#ifdef CONFIG_RING_SERVICE
extern void ring_rx_handler(void);
#endif

/**
 * @brief QCSPI interrupt handler
 *
 * @param[in] dev  Pointer to the qcspi driver device structure.
 */
static void spi_qcc730_spis_isr(const struct device *dev)
{
	const struct spi_qcc730_cfg *cfg = dev->config;
	QCSPI_SLAVE_BASE_qcspi_slave_Type *regs = cfg->regs;
	struct spi_qcc730_data *data = dev->data;
	uint32_t int_status;

	int_status = regs->QCSPI_SLAVE_R_SPI_SLAVE_IRQ_STATUS.reg;

	/* Clear the interrupt */
	regs->QCSPI_SLAVE_R_SPI_SLAVE_IRQ_CLR.bit.HOST_INT0_IRQ_CLR = 1U;

#ifdef CONFIG_SPI_ASYNC
	if (data->ctx.callback != NULL) {
		data->ctx.callback(dev, 0, data->ctx.callback_data);
	}
#endif

	if (int_status & QCSPI_SLAVE_HOST_INT0_MASK) {
#ifdef CONFIG_RING_SERVICE
	/* Notify ring service if enabled */
	ring_rx_handler();
#endif	
	}
	
	if (regs->QCSPI_SLAVE_R_SPI_SLAVE_SW_RST_IRQ.bit.SW_RST_REQ_IRQ) {
		regs->QCSPI_SLAVE_R_SPI_SLAVE_SW_RESET.bit.SW_RESET = QCSPI_SLAVE_ENABLE;
		LOG_ERR("SPI RESET Interrupt triggered!");
	}

	//LOG_DBG("SPI Interrupt triggered!");
}

/**
 * @brief Initialize QCC730 qcspi driver
 *
 * @param[in] dev  Pointer to the qcspi driver device structure.
 *
 * @return 0 in case of success
 *         Negative error code otherwise
 */
static int spi_qcc730_spis_init(const struct device *dev)
{
	struct spi_qcc730_data *data = dev->data;
	const struct spi_qcc730_cfg *cfg = dev->config;
	PMU_BASE_pmu_Type *pmu = cfg->pmu;
	QCSPI_SLAVE_BASE_qcspi_slave_Type *regs = cfg->regs;
	int ret;

	/* check if gpio driver is ready */
	if (!device_is_ready(gpio_dev)) {
		LOG_ERR("GPIO driver not ready!");
		return -ENODEV;
	}

	/* Set pin modes */
	ret = gpio_pin_configure(gpio_dev, cfg->pin_num, GPIO_INPUT | GPIO_PULL_DOWN);
	if (ret != 0) {
		LOG_ERR("Pin configuration for qcspi failed wit code %d", ret);
		return ret;
	}

	/* Enable root clock */
	pmu->PMU_ROOT_CLK_ENABLE.bit.SPI_ROOT_CLK_ENABLE = 1U;

	if (pmu->PMU_ROOT_CLK_ENABLE.bit.SPI_ROOT_CLK_ENABLE != 1U) {
		return -EADDRNOTAVAIL;
	}

	/* Enable interrupt (for example corresponding NVIC bit) */
	IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority), spi_qcc730_spis_isr,
		    DEVICE_DT_INST_GET(0), 0);
	irq_enable(DT_INST_IRQN(0));

	/* This register needs to be written first to unlock
	 * write access to BOOT_STRAP_CONFIGURATION_STATUS */
	pmu->PMU_BOOT_STRAP_CONFIG_SECURE.reg = QCC730_QCSPI_SLAVE_FR_BOOT_STRAP_VALUE;

	/* Disabling DWSPI, Enabling QcSPI */
	pmu->PMU_BOOT_STRAP_CONFIGURATION_STATUS.bit.CFG_SPISLAVE_SELECT = 1U;

	/* This register needs to be written first to unlock
	 * write access to BOOT_STRAP_CONFIGURATION_STATUS */
	pmu->PMU_BOOT_STRAP_CONFIG_SECURE.reg = QCC730_QCSPI_SLAVE_FR_BOOT_STRAP_VALUE;

	/* Enable SPI slave */
	pmu->PMU_BOOT_STRAP_CONFIGURATION_STATUS.bit.CFG_SPI_ENABLE = 1U;

	/* Disabling Serial Synchronous Interface */
	dwspi_regs->DWSPI_SLAVE_SSIENR.bit.SSI_EN = 0U;

	/* 1.  Disable QcSPI Slave Core */
	regs->QCSPI_SLAVE_R_SPI_SLAVE_CONFIG.bit.CORE_DIS = 1U;

	/**
	 * 2.  Configuring QcSpi
	 * Keeping default values except N_DUMMY and Address_byte_length
	 * Enabling WP_DIS and SEQMOD
	 * (5 dummy bytes, 0x05<<24 and 4 address bytes, ~(2<<15))
	 */
	regs->QCSPI_SLAVE_R_SPI_SLAVE_CONFIG.reg = QCC730_QCSPI_HOST_CONFIG;

	/* 3.   Re-Enabling QcSPI Slave Core */
	regs->QCSPI_SLAVE_R_SPI_SLAVE_CONFIG.bit.CORE_DIS = 0U;

	/* Setting QcSPI priority over AHB to the highest, to reduce memory access latencies,
	   to use reasonable number of dummy bytes */
	cdahb_regs->CDAHB_CDAHB_SPI_S_PL.bit.PRIORITY = QCC730_QCSPI_AHB_PRI_CONFIG;

	/* Enable HOST_INT0 interrupt */
	regs->QCSPI_SLAVE_R_SPI_SLAVE_IRQ_EN.bit.HOST_INT0_IRQ_EN = 1U;

	/* Enable SW_RESET_IRQ_EN interrupt */
	regs->QCSPI_SLAVE_R_SPI_SLAVE_IRQ_EN.bit.SW_RESET_IRQ_EN = 1U;

	spi_context_unlock_unconditionally(&data->ctx);

	LOG_DBG("qcSPI slave driver initialized");

	return 0;
}

#ifdef CONFIG_PM_DEVICE
static int spi_qcc730_spis_deinit(const struct device *dev)
{
	const struct spi_qcc730_cfg *cfg = dev->config;
	PMU_BASE_pmu_Type *pmu = cfg->pmu;

	/* Disable root clock */
	pmu->PMU_ROOT_CLK_ENABLE.bit.SPI_ROOT_CLK_ENABLE = 0U;

	if (pmu->PMU_ROOT_CLK_ENABLE.bit.SPI_ROOT_CLK_ENABLE != 0U) {
		return -EADDRNOTAVAIL;
	}

	/* Disable interrupt */
	irq_disable(DT_INST_IRQN(0));

	/* This register needs to be written first to unlock
	 * write access to BOOT_STRAP_CONFIGURATION_STATUS */
	pmu->PMU_BOOT_STRAP_CONFIG_SECURE.reg = QCC730_QCSPI_SLAVE_FR_BOOT_STRAP_VALUE;

	/* Disable SPI slave */
	pmu->PMU_BOOT_STRAP_CONFIGURATION_STATUS.bit.CFG_SPI_ENABLE = 0U;

	return 0;
}

static int spi_qcc730_spis_pm_action(const struct device *dev, enum pm_device_action action)
{
	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		spi_qcc730_spis_deinit(dev);
		break;
	case PM_DEVICE_ACTION_RESUME:
		spi_qcc730_spis_init(dev);
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}
#endif /* CONFIG_PM_DEVICE */

static DEVICE_API(spi, spi_qcc730_spis_driver_api) = {
	.transceive = spi_qcc730_spis_transceive,
	.release = spi_qcc730_spis_release,
};

#define SPI_QCC730_DEVICE(n)                                                                       \
	static const struct spi_qcc730_cfg spi_qcc730_cfg_##n = {                                  \
		.regs = (QCSPI_SLAVE_BASE_qcspi_slave_Type *)DT_INST_REG_ADDR(n),                  \
		.pmu = (PMU_BASE_pmu_Type *)DT_REG_ADDR(DT_NODELABEL(pmu)),                        \
		.pin_num = DT_INST_PROP(n, pin_num),                                               \
	};                                                                                         \
                                                                                                   \
	static struct spi_qcc730_data spi_qcc730_data_##n = {                                      \
		SPI_CONTEXT_INIT_LOCK(spi_qcc730_data_##n, ctx),                                   \
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(n), ctx)};                             \
                                                                                                   \
        PM_DEVICE_DT_INST_DEFINE(n, spi_qcc730_spis_pm_action);                                    \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, spi_qcc730_spis_init, PM_DEVICE_DT_INST_GET(n),                   \
			      &spi_qcc730_data_##n,                                                \
			      &spi_qcc730_cfg_##n,                                                 \
			      POST_KERNEL, CONFIG_SPI_INIT_PRIORITY,                               \
			      &spi_qcc730_spis_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SPI_QCC730_DEVICE)
