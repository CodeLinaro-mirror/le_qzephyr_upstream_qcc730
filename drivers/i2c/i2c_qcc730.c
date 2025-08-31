/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qcom_qcc730_i2c

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/dt-bindings/i2c/i2c.h>
#include "ferm_i2c.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(i2c_qcc730, CONFIG_I2C_LOG_LEVEL);

#include "../../../zephyr/drivers/i2c/i2c-priv.h"

struct i2c_qcc730_config {
	PMU_BASE_pmu_Type *pmu;
	uint32_t freq;
	uint32_t addr;
};

struct i2c_qcc730_data {
	/* i2c_dev */
	i2c_hal *hal;
	i2c_cap cap;
	i2c_sdk_config config;
	i2c_xfr xfr;
	uint32_t state;
	i2c_stats stats;
	struct k_sem lock_sem;
#ifdef CONFIG_I2C_QCC730_INTERRUPT_SUPPORT
	uint8_t curr_operation;
	struct k_sem send_sem;
	struct k_sem receive_sem;
	uint32_t interrupt_error;
#endif
};

/* Designware Component Type number = 0x44_57_01_40.
This assigned unique hex value is constant and is derived
from the two ASCII letters 'DW' followed by a 16-bit unsigned
number.*/
#define QWLAN_I2C_I2C_IC_COMP_TYPE_DEFAULT 0x44570140

const i2c_scl_ht_lt scl_default[I2C_SPEED_NUM] = {
	{I2C_STD_SPEED_DEFAULT, I2C_STD_MODE, 4730, 5270},
	{I2C_FAST_SPEED_DEFAULT, I2C_FAST_MODE, 1340, 1160},
	{I2C_FAST_PLUS_SPEED_DEFAULT, I2C_FAST_MODE, 500, 500},
	{I2C_HIGH_SPEED_DEFAULT, I2C_HIGH_MODE, 200, 300},
};

/**
 * @brief Get the capability of Fermion I2C module.
 *
 * @param[in/out] data  Pointer to the I2C driver device data.
 *
 */
static inline void i2c_qcc730_get_cap(struct i2c_qcc730_data *data)
{
	i2c_cap *cap = &data->cap;

	/* Get the default value on initialization:
	 * supported speed mode, spk lenth and tx/rx fifo depth in hardware. */
	i2c_hal_max_speed_mode(data->hal, &(cap->support_mode));
	i2c_hal_spklen(data->hal, &(cap->fs_spklen), &(cap->hs_spklen));
	i2c_hal_tx_rx_fifo_depth(data->hal, &(cap->tx_depth), &(cap->rx_depth));

	/* RX fifo threshold as 0 to trigger RX interruption whenever data is arrived. */
	cap->rx_tl = 0;
	/* TX fifo threshold as 0 to trigger TX empty interruption only when tx fifo is truly empty
	 */
	cap->tx_tl = 0;

	return;
}

/**
 * @brief Setting I2CM platform related configuration.
 *
 * 1) I2C clock configuration
 * 2) I2C bootstrap configuration
 * 3) I2C interruption enable/disable in SOC level
 *
 * @param[in] dev  Pointer to the I2C driver device.
 * @param[in] enable indicator if platform should be enabled or disabled
 *
 * @return 0 in case of success
 *         I2C_ERROR_BOOTSTRAP_CFG_FAIL in case of Bootstrap configuration fail
 *         I2C_ERROR_BUS_CLK_CFG_FAIL in case of clock configuration fail
 */
static i2c_status i2c_qcc730_platform(const struct device *dev, uint8_t enable)
{
	const struct i2c_qcc730_config *config = (const struct i2c_qcc730_config *)dev->config;
	PMU_BASE_pmu_Type *pmu = config->pmu;
#ifdef CONFIG_I2C_QCC730_INTERRUPT_SUPPORT
	uint32_t value;
#endif

	if (enable) {
		pmu->PMU_ROOT_CLK_ENABLE.bit.I2C_ROOT_CLK_ENABLE = enable;
		pmu->PMU_BOOT_STRAP_CONFIG_SECURE.reg = FERM_BOOT_STRAP_VALUE;
		pmu->PMU_BOOT_STRAP_CONFIGURATION_STATUS.bit.CFG_I2C_ENABLE = enable;

#ifdef CONFIG_I2C_QCC730_INTERRUPT_SUPPORT
		value = *((volatile unsigned long *)(NT_NVIC_ISER0));
		value |= BIT(I2C_IRQn);
		*((volatile unsigned long *)(NT_NVIC_ISER0)) = ((unsigned long)value);
#endif
	} else {
#ifdef CONFIG_I2C_QCC730_INTERRUPT_SUPPORT
		value = *((volatile unsigned long *)(NT_NVIC_ICER0));
		value |= BIT(I2C_IRQn);
		*((volatile unsigned long *)(NT_NVIC_ICER0)) = ((unsigned long)value);
#endif
		pmu->PMU_BOOT_STRAP_CONFIG_SECURE.reg = FERM_BOOT_STRAP_VALUE;
		pmu->PMU_BOOT_STRAP_CONFIGURATION_STATUS.bit.CFG_I2C_ENABLE = enable;
		pmu->PMU_ROOT_CLK_ENABLE.bit.I2C_ROOT_CLK_ENABLE = enable;
	}

	if (pmu->PMU_BOOT_STRAP_CONFIGURATION_STATUS.bit.CFG_I2C_ENABLE != enable) {
		return I2C_ERROR_BOOTSTRAP_CFG_FAIL;
	}

	if (pmu->PMU_ROOT_CLK_ENABLE.bit.I2C_ROOT_CLK_ENABLE != enable) {
		return I2C_ERROR_BUS_CLK_CFG_FAIL;
	}

	return 0;
}

/**
 * @brief Get I2C mode related configuration.
 *
 * @param[in] cap  Pointer to the I2C structure with device capabilities.
 * @param[out] sdk_config Pointer to the configuration to be updated.
 * @param[in] freq Device frequency in kHz
 *
 * * @return 0 in case of success
 *           -EINVAL in case of frequency provided is not supported
 */
static i2c_status i2c_qcc730_get_scl_hcnt_lcnt(i2c_cap *cap, i2c_sdk_config *sdk_config,
					       const uint32_t freq)
{
	uint8_t i, spklen, find = 0;
	uint32_t low_time, high_time;

	for (i = 0; i < I2C_SPEED_NUM; i++) {
		if (freq == scl_default[i].freq) {
			sdk_config->mode = scl_default[i].mode;
			low_time = scl_default[i].low_time;
			high_time = scl_default[i].high_time;

			find = 1;
			break;
		}
	}

	if (find == 1) {
		if (sdk_config->mode == I2C_HIGH_MODE) {
			spklen = cap->hs_spklen;
		} else {
			spklen = cap->fs_spklen;
		}

		sdk_config->hcnt = HCNT_CAL(high_time, cap->clk_khz, spklen);
		sdk_config->lcnt = LCNT_CAL(low_time, cap->clk_khz);
	} else {
		return -EINVAL;
	}

	return 0;
}

#ifdef CONFIG_I2C_QCC730_INTERRUPT_SUPPORT
static void i2c_qcc730_isr(const struct device *dev)
{
	struct i2c_qcc730_data *data = (struct i2c_qcc730_data *)dev->data;
	uint32_t unused_value;
	uint32_t ic_intr_stat;

	/* Read Interrupt Status Register to determine what caused interrupt */
	i2c_hal_intr_stat(data->hal, &ic_intr_stat);

	if (0 == (ic_intr_stat & I2C_INTR_ERROR)) {
		/* Success - release the semaphore */
		if (I2C_MSG_READ == data->curr_operation) {
			k_sem_give(&data->receive_sem);
		} else {
			k_sem_give(&data->send_sem);
		}
	} else {
		/* Error - save error cause */
		data->interrupt_error = ic_intr_stat;
	}

	/* Disable all the interrupts */
	i2c_hal_intr_mask(data->hal, 0);
	i2c_hal_intr_clear_all(data->hal, &unused_value);
}
#endif

/**
 * @brief Initialize QCC730 i2c driver
 *
 * @param[in] dev  Pointer to the I2C driver device structure.
 *
 * @return 0 in case of success
 *         -ENODEV in case of failure in platform enabling
 *         -EINVAL in case of failure in DesignWare register read
 */
static int i2c_qcc730_init(const struct device *dev)
{
	uint32_t comp_type;
	uint8_t enable = 1;
	i2c_status status;
	struct i2c_qcc730_data *data = (struct i2c_qcc730_data *)dev->data;

	status = i2c_qcc730_platform(dev, enable);

	if (0 != status) {
		printf("i2c_open platform enable failed %d\n", status);
		return -ENODEV;
	}

	data->hal = (i2c_hal *)(QCC730V2_I2C_BASE_BASE);

	i2c_hal_comp_type(data->hal, &comp_type);

	/* Verify that we have a valid DesignWare register first */
	if (comp_type != QWLAN_I2C_I2C_IC_COMP_TYPE_DEFAULT) {
		printf("i2c_open I2C comparsion register type failure\n");
		memset(data, 0, sizeof(struct i2c_qcc730_data));
		return -EINVAL;
	}

	i2c_qcc730_get_cap(data);

#ifdef CONFIG_I2C_QCC730_INTERRUPT_SUPPORT
	IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority), i2c_qcc730_isr,
		    DEVICE_DT_INST_GET(0), 0);
	irq_enable(DT_INST_IRQN(0));

	k_sem_init(&data->send_sem, 0, 1);
	k_sem_init(&data->receive_sem, 0, 1);
#endif
	k_sem_init(&data->lock_sem, 1, 1);

	data->state = I2C_INIT;

	return 0;
}

static int i2c_qcc730_configure(const struct device *dev, uint32_t dev_config)
{
	struct i2c_qcc730_data *data = (struct i2c_qcc730_data *)dev->data;
	i2c_sdk_config *sdk_config = &data->config;
	i2c_hal *hal = data->hal;
	i2c_cap *cap = &data->cap;
	uint32_t unused_value;
	i2c_status status;
	uint32_t freq;

	if ((data->state & I2C_INIT) == 0) {
		return -ENODEV;
	}

	if (!(dev_config & I2C_MODE_CONTROLLER)) {
		return -EINVAL;
	}

	switch (I2C_SPEED_GET(dev_config)) {
	case I2C_SPEED_STANDARD:
		freq = I2C_STD_SPEED_DEFAULT;
		break;
	case I2C_SPEED_FAST:
		freq = I2C_FAST_SPEED_DEFAULT;
		break;
	case I2C_SPEED_FAST_PLUS:
		freq = I2C_FAST_PLUS_SPEED_DEFAULT;
		break;
	case I2C_SPEED_HIGH:
		freq = I2C_HIGH_SPEED_DEFAULT;
		break;
	default:
		return -EINVAL;
	}

	status = i2c_qcc730_get_scl_hcnt_lcnt(cap, sdk_config, freq);

	if (0 != status) {
		data->state &= ~I2C_SETUP;
		return status;
	}

	i2c_hal_disable(hal);

	i2c_hal_intr_mask(hal, 0);

	/* Interrupts are being cleared just by register read */
	i2c_hal_intr_clear_all(hal, &unused_value);

	i2c_hal_slave_disable(hal, 1);
	i2c_hal_restart_enable(hal, 1);
	i2c_hal_slave_10bit_addr_enable(hal, 0);
	i2c_hal_master_10bit_addr_enable(hal, 0);
	i2c_hal_master_enable(hal, 1);

	i2c_hal_speed_mode(hal, sdk_config->mode);

	if (sdk_config->mode == I2C_STD_MODE) {
		i2c_hal_ss_scl_config(hal, sdk_config->lcnt, sdk_config->hcnt);
	} else if (sdk_config->mode == I2C_FAST_MODE) {
		i2c_hal_fs_scl_config(hal, sdk_config->lcnt, sdk_config->hcnt);
	} else {
		i2c_hal_hs_scl_config(hal, sdk_config->lcnt, sdk_config->hcnt);
	}

	i2c_hal_tl_rl_config(hal, cap->tx_tl, cap->rx_tl);

	data->state |= I2C_SETUP;

	i2c_hal_enable(hal);

	return 0;
}

/**
 * @brief Function for setting device address.
 *
 * @param[in] dev  Pointer to the I2C driver device.
 * @param[in] addr Address to be set.
 *
 */
static void i2c_qcc730_config_addr(const struct device *dev, uint16_t addr)
{
	struct i2c_qcc730_data *data = (struct i2c_qcc730_data *)dev->data;
	i2c_hal *hal = data->hal;

	i2c_hal_disable(hal);

	i2c_hal_tar_write(hal, addr);

	i2c_hal_enable(hal);
}

#ifdef CONFIG_I2C_QCC730_INTERRUPT_SUPPORT
/**
 * @brief Function for printing interrupt error cause basing on IC_INTR_STAT register value
 *
 * @param[in] dev  Pointer to the I2C driver device.
 */
void i2c_qcc730_print_error_cause(const struct device *dev)
{
	struct i2c_qcc730_data *data = (struct i2c_qcc730_data *)dev->data;

	printf("I2C interrupt error: ");

	if (data->interrupt_error & I2C_INTR_TX_ABRT) {
		printf("transmitter unable to complete intended actions \n");
	} else if (data->interrupt_error & I2C_INTR_TX_OV) {
		printf("transmit buffer filled to IC_TX_BUFFER_DEPTH and another write issued \n");
	} else if (data->interrupt_error & I2C_INTR_RX_OV) {
		printf("receive buffer filled to IC_RX_BUFFER_DEPTH and additional byte was "
		       "received \n");
	} else if (data->interrupt_error & I2C_INTR_RX_UN) {
		printf("attempt to read receive buffer when it is empty \n");
	} else {
		printf("Timeout \n");
	}
}
#endif

/**
 * @brief Receive I2C message
 *
 * @param[in] dev  Pointer to the I2C driver device.
 * @param[in/out] msg  Message to be received and its parameters
 *
 * @return 0 in case of successful message read
 *         -ETIMEDOUT in case of a timeout
 */
static i2c_status i2c_qcc730_data_recv(const struct device *dev, struct i2c_msg *msg)
{
	struct i2c_qcc730_data *data = (struct i2c_qcc730_data *)dev->data;
	i2c_hal *hal = data->hal;
	int len = msg->len;
	int pos = 0;

	/* For each byte we want to receive, we need to write a read command to the controller */
	while (len > 0) {
		uint32_t read_cmd = I2C_DATA_CMD_READ;

		/* Add RESTART flag if needed for this byte */
		if ((pos == 0) && (msg->flags & I2C_MSG_RESTART)) {
			read_cmd |= I2C_DATA_CMD_RESTART;
		}

		/* Add STOP flag if this is the last byte and STOP is requested */
		if ((len == 1) && (msg->flags & I2C_MSG_STOP)) {
			read_cmd |= I2C_DATA_CMD_STOP;
		}

#ifdef CONFIG_I2C_QCC730_INTERRUPT_SUPPORT
		/* Enable RX interrupts */
		i2c_hal_intr_mask(data->hal, I2C_INTR_RX);
#endif

		/* Write the read command to initiate the read */
		i2c_hal_data_cmd_write(hal, read_cmd);

#ifdef CONFIG_I2C_QCC730_INTERRUPT_SUPPORT
		if (k_sem_take(&data->receive_sem, K_MSEC(CONFIG_I2C_QCC730_TIMEOUT_MS)) != 0) {
			i2c_qcc730_print_error_cause(dev);
			return -ETIMEDOUT;
		};
#else
		/* Wait for data to be available in RX FIFO */
		uint32_t start_time = k_uptime_get_32();
		while (!i2c_hal_rx_fifo_not_empty(hal)) {
			if (k_uptime_get_32() - start_time > CONFIG_I2C_QCC730_TIMEOUT_MS) {
				return -ETIMEDOUT;
			}
		}
#endif
		/* Read the received byte */
		msg->buf[pos] = (uint8_t)i2c_hal_data_cmd_read(hal);
		pos++;
		len--;
	}

	return 0;
}

/**
 * @brief Send I2C message
 *
 * @param[in] dev  Pointer to the I2C driver device.
 * @param[in/out] msg  Message to be sent and its parameters
 *
 * @return 0 if message was successfully sent
 *         -ETIMEDOUT in case of a timeout
 */
static i2c_status i2c_qcc730_data_write(const struct device *dev, struct i2c_msg *msg)
{
	struct i2c_qcc730_data *data = (struct i2c_qcc730_data *)dev->data;
	i2c_hal *hal = data->hal;
	int len = msg->len;
	int pos = 0;

	while (len > 0) {
		uint32_t write_cmd = 0;
		uint32_t start_time = k_uptime_get_32();
		while (!i2c_hal_tx_fifo_not_full(hal)) {
			if (k_uptime_get_32() - start_time > CONFIG_I2C_QCC730_TIMEOUT_MS) {
				return -ETIMEDOUT;
			}
		}

		/* Add RESTART flag if needed for this byte */
		if ((pos == 0) && (msg->flags & I2C_MSG_RESTART)) {
			write_cmd |= I2C_DATA_CMD_RESTART;
		}

		/* Add STOP flag if this is the last byte and STOP is requested */
		if ((len == 1) && (msg->flags & I2C_MSG_STOP)) {
			write_cmd |= I2C_DATA_CMD_STOP;
		}

		/* Write the data byte along with any flags */
		write_cmd |= msg->buf[pos];
		i2c_hal_data_cmd_write(hal, write_cmd);

#ifdef CONFIG_I2C_QCC730_INTERRUPT_SUPPORT
		/* Enable TX interrupts */
		i2c_hal_intr_mask(data->hal, I2C_INTR_TX);

		if (k_sem_take(&data->send_sem, K_MSEC(CONFIG_I2C_QCC730_TIMEOUT_MS)) != 0) {
			i2c_qcc730_print_error_cause(dev);
			return -ETIMEDOUT;
		}
#else
		/* Wait until TX FIFO has space */
		while (!i2c_hal_tx_fifo_not_full(hal))
			;
		k_sleep(K_USEC(1));
#endif
		pos++;
		len--;
	}

	return 0;
}

static int i2c_qcc730_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t num_msgs,
			       uint16_t addr)
{
	struct i2c_qcc730_data *data = (struct i2c_qcc730_data *)dev->data;
	struct i2c_qcc730_config *config = (struct i2c_qcc730_config *)dev->config;
	uint32_t bitrate_cfg = i2c_map_dt_bitrate(config->freq);


	if (!num_msgs){
		return 0;
	}

	if (msgs->flags & I2C_MSG_ADDR_10_BITS) {
		printf("10bit addressing is not supported\n");
		return -ENOTSUP;
	}

	/* Configure the device (without address setting) */
	i2c_qcc730_configure(dev, I2C_MODE_CONTROLLER | bitrate_cfg);

	/* Since device here is the TWI driver (not particular sensor)
	 * we need to set address separately to configure function */
	i2c_qcc730_config_addr(dev, addr);

	k_sem_take(&data->lock_sem, K_FOREVER);
	msgs[0].flags |= I2C_MSG_RESTART;
	for (int i = 0; i < num_msgs; i++) {
		if (i > 0) {
			if ((msgs[i - 1].flags & (I2C_MSG_STOP | I2C_MSG_READ))) {
				msgs[i].flags |= I2C_MSG_RESTART;
			}
		}

		int ret;

		if (msgs[i].flags & I2C_MSG_READ) {
#ifdef CONFIG_I2C_QCC730_INTERRUPT_SUPPORT
			data->curr_operation = I2C_MSG_READ;
#endif
			ret = i2c_qcc730_data_recv(dev, &msgs[i]);
		} else {
#ifdef CONFIG_I2C_QCC730_INTERRUPT_SUPPORT
			data->curr_operation = I2C_MSG_WRITE;
#endif
			ret = i2c_qcc730_data_write(dev, &msgs[i]);
		}

		if (0 != ret) {
			k_sem_give(&data->lock_sem);
			return ret;
		}
	}

	k_sem_give(&data->lock_sem);

	return 0;
}

static const struct i2c_driver_api i2c_qcc730_api = {
	.configure = i2c_qcc730_configure,
	.transfer = i2c_qcc730_transfer,
};

#define DEFINE_I2C_QCC730(n)                                                                       \
                                                                                                   \
	static struct i2c_qcc730_data i2c_data_##n = {                                             \
		.hal = (i2c_hal *)DT_REG_ADDR(DT_NODELABEL(i2c0)),                                 \
		.cap =                                                                             \
			{                                                                          \
				.clk_khz = DT_INST_PROP(n, periph_clock_frequency_khz),            \
			},                                                                         \
	};                                                                                         \
                                                                                                   \
	static const struct i2c_qcc730_config i2c_cfg_##n = {                                      \
		.pmu = (PMU_BASE_pmu_Type *)DT_REG_ADDR(DT_NODELABEL(pmu)),                        \
		.freq = (DT_INST_PROP(n, clock_frequency)),                                        \
		.addr = DT_INST_REG_ADDR(n),                                                       \
	};                                                                                         \
                                                                                                   \
	I2C_DEVICE_DT_INST_DEFINE(n, i2c_qcc730_init, NULL, &i2c_data_##n, &i2c_cfg_##n,           \
				  POST_KERNEL, CONFIG_I2C_INIT_PRIORITY, &i2c_qcc730_api);

DT_INST_FOREACH_STATUS_OKAY(DEFINE_I2C_QCC730)
