/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qcom_qcc730_uart

#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>
#include <zephyr/pm/device.h>
#include "zephyr/dt-bindings/serial/uart_qcom_qcc730.h"
#include "ferm_uart_hal.h"

#include "soc.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(uart_qcc730, CONFIG_UART_LOG_LEVEL);

/*
 * Timeout for uart_qcc730_putchar() waiting for THRE (TX FIFO has space).
 * At 60 MHz with ~5 cycles/iteration, 3000 iterations ≈ 250 µs, which is
 * well above the ~87 µs required to transmit one byte at 115200 baud.
 * Previously 1000 iterations (≈83 µs) was shorter than one byte-time and
 * caused silent byte drops during high-throughput poll_out loops.
 */
#define UART_TRANS_TIME_OUT   3000
#define DIVISOR_DLL(divisor)  (divisor & 0xff)
#define DIVISOR_DLH(divisor)  ((divisor >> 8) & 0xff)
#define BAUDRATE_NUM_MAX      9
#define UART_TX_EMPTY_WAIT_US 10000

struct uart_qcc730_config {
	UART_BASE_uart_Type *uart_hal_regs;
	PMU_BASE_pmu_Type *pmu;
	const struct pinctrl_dev_config *pin_cfg;
	struct reset_dt_spec reset;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
};

struct uart_qcc730_data {
	struct uart_config *uart_cfg;
	struct ring_buf *rx_ringbuf;
	struct k_spinlock lock;
	bool uart_initialized;
};

typedef struct {
	uint32_t baudrate;
	uint32_t divisor;
} uart_qcc730_baudrate_divisor;

// Baudrate = (serial_clk_freq) / (16 * Divisor)
/* Hardware issue limits the peformace of UART, speeds lower than 115200 DO NOT work properly */
static const uart_qcc730_baudrate_divisor baudrate_table[BAUDRATE_NUM_MAX] = {
	{921600, 0x04}, {460800, 0x08}, {230400, 0x10}, {115200, 0x20}, {57600, 0x40},
	{38400, 0x60},  {19200, 0xc0},  {9600, 0x180},  {4800, 0x300},
};

/**
 * @brief Gets the divisor value for a given UART baudrate.
 *
 * @param baudrate baudrate.
 * @param divisor  Pointer to store the resulting divisor value.
 *
 * @return 0 on success, or a negative errno code on failure.
 */
static inline int32_t uart_qcc730_get_divisor_by_baudrate(uint32_t baudrate, uint32_t *divisor)
{
	if (divisor == NULL) {
		return -EINVAL;
	}

	for (int i = 0; i < sizeof(baudrate_table) / sizeof(baudrate_table[0]); i++) {
		if (baudrate_table[i].baudrate == baudrate) {
			// On success, write to the pointer and return 0
			*divisor = baudrate_table[i].divisor;
			return 0; // Success
		}
	}
	return -EINVAL; // Invalid argument (baudrate not found)
}

/**
 * @brief This function returns parity configs available in hardware to configure (see qapi_uart.h)
 *
 * @param parity enum uart_config_parity
 * @param parity_cfg pointer to int where configuration value is to be stored to be set in hardware
 *
 * @return int32_t 0 on success, or a negative errno code on failure.
 */
static inline int32_t uart_qcc730_get_parity(enum uart_config_parity parity, uint32_t *parity_cfg)
{
	int32_t ret = 0;

	switch (parity) {
	case UART_CFG_PARITY_NONE:
		*parity_cfg = 0;
		break;
	case UART_CFG_PARITY_ODD:
		*parity_cfg = 1;
		break;
	case UART_CFG_PARITY_EVEN:
		*parity_cfg = 2;
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

/**
 * @brief This function returns stop bits available in hardware to configure (see qapi_uart.h)
 *
 * @param stopbits num uart_config_stop_bits from uart.h
 * @param stopbit_cfg pointer to int where configuration value is to be stored to be set in hardware
 *
 * @return int32_t success = 0 or errno otherwise
 */
static inline int32_t uart_qcc730_get_stop_bits(enum uart_config_stop_bits stopbits,
						uint32_t *stopbit_cfg)
{
	int32_t ret = 0;

	switch (stopbits) {
	case UART_CFG_STOP_BITS_1:
		*stopbit_cfg = 0;
		break;
	case UART_CFG_STOP_BITS_1_5:
	case UART_CFG_STOP_BITS_2:
		*stopbit_cfg = 1;
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

/**
 * @brief This function returns data bits available in hardware to configure (see qapi_uart.h)
 *
 * @param data_bits enum uart_config_data_bits from uart.h
 *
 * @return int mapped value available to be set in hardware
 */
static inline int32_t uart_qcc730_get_data_bits(enum uart_config_data_bits data_bits,
						uint32_t *databit_cfg)
{
	int32_t ret = 0;

	switch (data_bits) {
	case UART_CFG_DATA_BITS_5:
		*databit_cfg = 0;
		break;
	case UART_CFG_DATA_BITS_6:
		*databit_cfg = 1;
		break;
	case UART_CFG_DATA_BITS_7:
		*databit_cfg = 2;
		break;
	case UART_CFG_DATA_BITS_8:
		*databit_cfg = 3;
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

/**
 * @brief This function updates the instance data->uart_cfg containing UART configurations
 *
 * @param dev pionter to uart device instance
 * @param new_cfg pointer to the new uart_config struct
 *
 * @return int 0 on success errno otherwise
 */
static inline int uart_qcc730_update_config(const struct device *dev,
					    const struct uart_config *new_cfg)
{
	int ret = 0;
	struct uart_qcc730_data *data = dev->data;

	if (!dev || !new_cfg) {
		return -EINVAL;
	}

	// Update the struct value at the address pointed by data->uart_cfg
	*data->uart_cfg = *new_cfg;

	return ret;
}

/**
 * @brief Needed in pre-kernel init functions for delay
 *
 * @param n number of cycles to delay
 */
static inline void uart_qcc730_nop_delay(uint32_t n)
{
	uint32_t nop_count = 0;
	for (nop_count = 0; nop_count < n; nop_count++) {
		__asm volatile(" nop \n");
	}
}

#ifdef CONFIG_PM_DEVICE
/* Platform enable/disable, used by init and PM funcitons. */
static int uart_qcc730_platform(const struct device *dev, uint8_t enable)
{
	const struct uart_qcc730_config *cfg = dev->config;
	struct uart_qcc730_data *data = dev->data;
	int ret = 0;

	if (enable) {

		/* Apply default pinctrl state */
		ret = pinctrl_apply_state(cfg->pin_cfg, PINCTRL_STATE_DEFAULT);
		if (ret < 0) {
			LOG_ERR("Failed to apply default pinctrl state (%d)", ret);
			return ret;
		}

		if (cfg->clock_dev) {
			if (!device_is_ready(cfg->clock_dev)) {
				return -ENODEV;
			}
			ret = clock_control_on(cfg->clock_dev, cfg->clock_subsys);
			if (ret < 0 && ret != -EALREADY) {
				return ret;
			}
		}

		irq_enable(DT_INST_IRQN(0));

		data->uart_initialized = true;
	} else {

		irq_disable(DT_INST_IRQN(0));

		if (cfg->clock_dev) {
			ret = clock_control_off(cfg->clock_dev, cfg->clock_subsys);
			if (ret < 0) {
				LOG_ERR("Error turning UART clock off for sleep (%d)", ret);
				return ret;
			}
		}

		/* Apply sleep pinctrl state */
		ret = pinctrl_apply_state(cfg->pin_cfg, PINCTRL_STATE_SLEEP);
		if (ret < 0) {
			LOG_ERR("Failed to apply sleep pinctrl state (%d)", ret);
		}

		data->uart_initialized = false;
	}

	return 0;
}

static int uart_qcc730_enable(const struct device *dev)
{
	return uart_qcc730_platform(dev, 1U);
}

static int uart_qcc730_suspend(const struct device *dev)
{
	return uart_qcc730_platform(dev, 0U);
}

static int uart_qcc730_pm_action(const struct device *dev, enum pm_device_action action)
{
	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		return uart_qcc730_enable(dev);
	case PM_DEVICE_ACTION_SUSPEND:
		return uart_qcc730_suspend(dev);
	default:
		return -ENOTSUP;
	}
}
#endif // CONFIG_PM_DEVICE

/**
 * @brief Sends the char out by writing it to QWLAN_UART_UART_RBR_REG register
 *
 * @param dev device instance
 * @param ch the byte to send out
 *
 * @return 0 on success, errno otherwise
 */
static int uart_qcc730_putchar(const struct device *dev, uint8_t ch)
{
	int ret;
	const struct uart_qcc730_config *cfg = dev->config;
	struct uart_qcc730_data *data = dev->data;
	UART_BASE_uart_Type *uart_hal_regs = cfg->uart_hal_regs;
	uint32_t timeout = UART_TRANS_TIME_OUT;

	/* Error while suspended/uninitialized */
	if (!data->uart_initialized) {
		return -EBUSY;
	}

	/*
	 * Wait for THRE (Transmitter Holding Register Empty, bit 5) — TX FIFO
	 * has space for a new byte. Using THRE instead of TEMPT (bit 6, full
	 * TX path empty) is correct for poll_out: we only need room to write
	 * one byte, not for the shift register to finish transmitting.
	 */
	while (uart_hal_regs->UART_UART_LSR.bit.THRE == 0 && timeout--)
		;

	// write to RBR only when THRE==1; otherwise quit with error
	if (uart_hal_regs->UART_UART_LSR.bit.THRE == 0) {
		// Timeout occurred, TX FIFO still full
		ret = -ETIMEDOUT; // Return timeout error code
	} else {
		uart_hal_regs->UART_UART_RBR.reg = (uint32_t)ch;
		ret = 0;
	}

	return ret;
}

/**
 * @brief QCC730 Device API poll_in for getting a byte of data from UART
 *
 * @param dev Devince Instance
 * @param p_char pointer to char byte where the polled in byte would be returned
 *
 * @return int 0 on Success, errno otherwise
 */
int uart_qcc730_poll_in(const struct device *dev, unsigned char *p_char)
{
	uint32_t ret;
	struct uart_qcc730_data *data = dev->data;

	/* Error while suspended/uninitialized */
	if (!data->uart_initialized) {
		return -EBUSY;
	}

	k_spinlock_key_t key = k_spin_lock(&data->lock);

	ret = ring_buf_get(data->rx_ringbuf, p_char, 1U);

	k_spin_unlock(&data->lock, key);

	return ret == 1U ? 0 : -ENODATA;
}

/**
 * @brief QCC730 Device API calling uart_qcc730_putchar implementation
 *
 * @param dev The device output is currently always the same
 * @param out_char The 8bit char value to send to uart. Just calls uart_qcc730_putchar
 */
void uart_qcc730_poll_out(const struct device *dev, unsigned char out_char)
{
	int ret = uart_qcc730_putchar(dev, out_char);

	if (ret != 0) {
		LOG_ERR("poll_out: byte 0x%02x dropped (err %d)", out_char, ret);
	}
}

/**
 * @brief QCC730 Device API allows runtime configuration of the uart peripheral from main
 * application
 *
 * @param dev uart device instance
 * @param new_uart_cfg pointer to uart_config struct with configurations of UART peripheral
 *
 * @return int 0 on success otherwise error code
 */
int uart_qcc730_configure(const struct device *dev, const struct uart_config *uart_cfg)
{
	const struct uart_qcc730_config *cfg = dev->config;
	struct uart_qcc730_data *data = dev->data;
	UART_BASE_uart_Type *uart_hal_regs = cfg->uart_hal_regs;
	int ret = 0, i = 0;
	uint32_t baud_divisor = 0U;
	uint32_t parity_cfg = 0U;
	uint32_t stopbit_cfg = 0U;
	uint32_t databit_cfg = 0U;

	if (uart_cfg == NULL) {
		LOG_ERR("uart_cfg is NULL");
		return -EINVAL;
	}

	if (!data->uart_initialized) {
		LOG_ERR("UART in sleep state or not initialized!");
		return -EBUSY;
	}

	if (data->uart_cfg == NULL) {
		LOG_ERR("Uart config is not initilized!");
		return -EINVAL;
	}

	// Validate config
	ret = uart_qcc730_get_divisor_by_baudrate(uart_cfg->baudrate, &baud_divisor);
	if (ret != 0) {
		LOG_ERR("Invalid baudrate for QCC730 UART");
		return ret;
	}
	ret = uart_qcc730_get_parity(uart_cfg->parity, &parity_cfg);
	if (ret != 0) {
		LOG_ERR("Invalid parity for QCC730 UART");
		return ret;
	}
	ret = uart_qcc730_get_stop_bits(uart_cfg->stop_bits, &stopbit_cfg);
	if (ret != 0) {
		LOG_ERR("Invalid stopbits for QCC730 UART");
		return ret;
	}
	ret = uart_qcc730_get_data_bits(uart_cfg->data_bits, &databit_cfg);
	if (ret != 0) {
		LOG_ERR("Invalid databits for QCC730 UART");
		return ret;
	}

	k_spinlock_key_t key = k_spin_lock(&data->lock);

	// Wait for TX path to be empty before reconfiguring
	for (i = 0; i < UART_TX_EMPTY_WAIT_US; i++) {
		if (uart_hal_regs->UART_UART_LSR.bit.TEMPT) {
			break;
		}
		k_busy_wait(1);
	}

	// Halt TX for configuration update
	uart_hal_regs->UART_UART_HTX.bit.HALT = 1U;

	uart_hal_divisor_access(uart_hal_regs, 1);
	uart_hal_divisor_low_cfg(uart_hal_regs, DIVISOR_DLL(baud_divisor));
	uart_hal_divisor_high_cfg(uart_hal_regs, DIVISOR_DLH(baud_divisor));
	uart_hal_divisor_access(uart_hal_regs, 0);

	if (uart_cfg->parity == UART_CFG_PARITY_NONE) {
		uart_hal_parity_enable(uart_hal_regs, 0);
	} else {
		uart_hal_parity_enable(uart_hal_regs, 1);
		uart_hal_event_parity_select(uart_hal_regs, parity_cfg);
	}
	uart_hal_stop_bits_cfg(uart_hal_regs, stopbit_cfg);
	uart_hal_data_bits_cfg(uart_hal_regs, databit_cfg);

	// Re-enable RX interrupt for poll-in workaround
	uart_hal_enable_intr_rx(uart_hal_regs);

	// Release TX halt
	uart_hal_regs->UART_UART_HTX.bit.HALT = 0U;

	k_spin_unlock(&data->lock, key);

	ret = uart_qcc730_update_config(dev, uart_cfg);
	if (ret != 0) {
		LOG_ERR("Update config error for QCC730");
	}

	return ret;
}

/**
 * @brief QCC730 Device API to get the current UART configuration
 *
 * @param dev pointer to device instance
 * @param current_cfg pointer to uart_config struct where the current configs will be returned
 *
 * @return int 0 on success and errno otherwise
 */
int uart_qcc730_config_get(const struct device *dev, struct uart_config *current_cfg)
{
	int ret = 0;
	struct uart_qcc730_data *data = dev->data;

	if (!dev || !current_cfg) {
		return -EINVAL;
	}

	if (!data || !data->uart_cfg) {
		return -ENOTSUP;
	}

	// Assign the current configs to current_cfg from data->uart_cfg
	*current_cfg = *data->uart_cfg;

	return ret;
}

/**
 * @brief UART interrupt handler
 *
 * @param dev device instance
 */
static void uart_qcc730_isr(const struct device *dev)
{
	struct uart_qcc730_data *data = dev->data;
	struct ring_buf *ringbuf = data->rx_ringbuf;
	const struct uart_qcc730_config *cfg = dev->config;
	UART_BASE_uart_Type *uart_hal_regs = cfg->uart_hal_regs;
	uint8_t byte_read = uart_hal_regs->UART_UART_RBR.bit.VALUE;

	ring_buf_put(ringbuf, &byte_read, 1);
}

/**
 * @brief This is init function port of uart_init from modules\hal\qcc730\uart\uart.c of qccsdk
 *
 * @param dev device instance
 *
 * @return int always returns 0
 */
static int uart_qcc730_init(const struct device *dev)
{
	struct uart_qcc730_data *data = dev->data;
	const struct uart_qcc730_config *cfg = dev->config;
	UART_BASE_uart_Type *uart_hal_regs = cfg->uart_hal_regs;
	uint32_t divisor = 0, current_baudrate = 0;
	uint32_t parity_cfg = 0U, stopbit_cfg = 0U, databit_cfg = 0U;
	int ret = 0;

	// Enable clock
	if (cfg->clock_dev) {
		if (!device_is_ready(cfg->clock_dev)) {
			return -ENODEV;
		}
		ret = clock_control_on(cfg->clock_dev, cfg->clock_subsys);
		if (ret < 0 && ret != -EALREADY) {
			return ret;
		}
	}

	// Reset peripheral
	ret = reset_line_toggle_dt(&cfg->reset);
	if (ret < 0) {
		return -EIO;
	}

	pinctrl_apply_state(cfg->pin_cfg, PINCTRL_STATE_DEFAULT);

	// Disable all UART interrupts
	uart_hal_regs->UART_UART_LCR.bit.DLAB = 0U;
	uart_hal_regs->UART_UART_DLH.reg = 0x00U;

	current_baudrate = data->uart_cfg->baudrate;
	ret = uart_qcc730_get_divisor_by_baudrate(current_baudrate, &divisor);
	if (ret != 0) {
		// Fallback to 115200
		current_baudrate = 115200U;
		(void)uart_qcc730_get_divisor_by_baudrate(current_baudrate, &divisor);
	}

	(void)uart_qcc730_get_parity(data->uart_cfg->parity, &parity_cfg);
	(void)uart_qcc730_get_stop_bits(data->uart_cfg->stop_bits, &stopbit_cfg);
	(void)uart_qcc730_get_data_bits(data->uart_cfg->data_bits, &databit_cfg);

	// Halt TX during initial setup
	uart_hal_regs->UART_UART_HTX.bit.HALT = 1U;

	// Set divisor, frame format, modem state, enable RX interrupt
	uart_hal_divisor_access(uart_hal_regs, 1);
	uart_hal_divisor_low_cfg(uart_hal_regs, DIVISOR_DLL(divisor));
	uart_hal_divisor_high_cfg(uart_hal_regs, DIVISOR_DLH(divisor));
	uart_hal_divisor_access(uart_hal_regs, 0);

	if (data->uart_cfg->parity == UART_CFG_PARITY_NONE) {
		uart_hal_parity_enable(uart_hal_regs, 0);
	} else {
		uart_hal_parity_enable(uart_hal_regs, 1);
		uart_hal_event_parity_select(uart_hal_regs, parity_cfg);
	}
	uart_hal_stop_bits_cfg(uart_hal_regs, stopbit_cfg);
	uart_hal_data_bits_cfg(uart_hal_regs, databit_cfg);

	// Release TX halt
	uart_hal_regs->UART_UART_HTX.bit.HALT = 0U;

	uart_hal_regs->UART_UART_MCR.reg = QCC730_UART_MCR_DEFAULT;

	IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority), uart_qcc730_isr,
		    DEVICE_DT_INST_GET(0), 0);
	irq_enable(DT_INST_IRQN(0));

	// Enable RX interrupt to as workaround for poll-in
	uart_hal_enable_intr_rx(uart_hal_regs);

	data->uart_initialized = true;

	return 0;
}

static DEVICE_API(uart, uart_qcc730_api) = {
	.poll_in = uart_qcc730_poll_in,
	.poll_out = uart_qcc730_poll_out,
	.configure = uart_qcc730_configure,
	.config_get = uart_qcc730_config_get,
};

#define UART_QCC730_INIT_DEVICE(n)                                                                 \
                                                                                                   \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
                                                                                                   \
	/* Configuration for uart instance "n" */                                                  \
	static const struct uart_qcc730_config uart_qcc730_cfg_##n = {                             \
		.uart_hal_regs = (UART_BASE_uart_Type *)DT_INST_REG_ADDR(n),                       \
		.pmu = (PMU_BASE_pmu_Type *)DT_REG_ADDR(DT_NODELABEL(pmu)),                        \
		.pin_cfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                      \
		.reset = RESET_DT_SPEC_INST_GET(n),                                                \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, id),                \
	};                                                                                         \
                                                                                                   \
	/* Default UART config for instance "n" */                                                 \
	static struct uart_config uart_qcc730_default_cfg_##n = {                                  \
		.baudrate = DT_INST_PROP_OR(n, current_speed, 115200),                             \
		.parity = DT_INST_ENUM_IDX_OR(n, parity, UART_CFG_PARITY_NONE),                    \
		.stop_bits = DT_INST_ENUM_IDX_OR(n, stop_bits, UART_CFG_STOP_BITS_1),              \
		.data_bits = DT_INST_ENUM_IDX_OR(n, data_bits, UART_CFG_DATA_BITS_8),              \
		.flow_ctrl = DT_INST_NODE_HAS_PROP(n, hw_flow_control)                             \
				     ? UART_CFG_FLOW_CTRL_RTS_CTS                                  \
				     : UART_CFG_FLOW_CTRL_NONE,                                    \
	};                                                                                         \
                                                                                                   \
	RING_BUF_DECLARE(uart_qcc730_rx_ringbuf_##n, DT_INST_PROP(n, rx_buffer_size));             \
                                                                                                   \
	/* Data for uart instance "n" */                                                           \
	static struct uart_qcc730_data uart_qcc730_data_##n = {                                    \
		.uart_cfg = &uart_qcc730_default_cfg_##n,                                          \
		.rx_ringbuf = &uart_qcc730_rx_ringbuf_##n,                                         \
	};                                                                                         \
                                                                                                   \
	PM_DEVICE_DT_INST_DEFINE(n, uart_qcc730_pm_action);                                        \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, uart_qcc730_init, PM_DEVICE_DT_INST_GET(n),                       \
			      &uart_qcc730_data_##n, &uart_qcc730_cfg_##n, PRE_KERNEL_1,           \
			      CONFIG_SERIAL_INIT_PRIORITY, &uart_qcc730_api);

DT_INST_FOREACH_STATUS_OKAY(UART_QCC730_INIT_DEVICE)
