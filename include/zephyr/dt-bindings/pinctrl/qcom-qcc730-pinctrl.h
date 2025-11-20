/*
 * Copyright (c) 2025 Qualcomm Technologies, Inc. and/or its subsidiaries
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_QCC730_COMMON_PINCTRL_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_QCC730_COMMON_PINCTRL_H_

/* Pin number field positions and masks */
#define QCOM_PINMUX_PIN_NUM_POS 24U
#define QCOM_PINMUX_PIN_NUM_MSK 0xFF000000U
#define QCOM_PINMUX_PIN_NUM(x)  (((x) << QCOM_PINMUX_PIN_NUM_POS) & QCOM_PINMUX_PIN_NUM_MSK)

/* UART Option field  */
#define QCOM_PINMUX_UART_OPT_POS 16U
#define QCOM_PINMUX_UART_OPT_MSK 0x00030000U
#define QCOM_PINMUX_UART_OPT(x)  (((x) << QCOM_PINMUX_UART_OPT_POS) & QCOM_PINMUX_UART_OPT_MSK)

/* Mode field input/output/peripheral */
#define QCOM_PINMUX_MODE_POS 14U
#define QCOM_PINMUX_MODE_MSK 0x0000C000U
#define QCOM_PINMUX_MODE(x)  (((x) << QCOM_PINMUX_MODE_POS) & QCOM_PINMUX_MODE_MSK)

/* Function selection field */
#define QCOM_PINMUX_FUNC_SEL_POS 8U
#define QCOM_PINMUX_FUNC_SEL_MSK 0x00003F00U
#define QCOM_PINMUX_FUNC_SEL(x)  (((x) << QCOM_PINMUX_FUNC_SEL_POS) & QCOM_PINMUX_FUNC_SEL_MSK)

/* GPIO Output Enable */
#define QCOM_PINMUX_OE_POS 6U
#define QCOM_PINMUX_OE_MSK 0x00000040U
#define QCOM_PINMUX_OE(x)  (((x) << QCOM_PINMUX_OE_POS) & QCOM_PINMUX_OE_MSK)

/* Pull Down */
#define QCOM_PINMUX_PULL_DOWN_POS 5U
#define QCOM_PINMUX_PULL_DOWN_MSK 0x00000020U
#define QCOM_PINMUX_PULL_DOWN(x)  (((x) << QCOM_PINMUX_PULL_DOWN_POS) & QCOM_PINMUX_PULL_DOWN_MSK)

/* Pull Up */
#define QCOM_PINMUX_PULL_UP_POS 4U
#define QCOM_PINMUX_PULL_UP_MSK 0x00000010U
#define QCOM_PINMUX_PULL_UP(x)  (((x) << QCOM_PINMUX_PULL_UP_POS) & QCOM_PINMUX_PULL_UP_MSK)

/* Driver Strength */
#define QCOM_PINMUX_DRIVER_STRENGTH_POS 2U
#define QCOM_PINMUX_DRIVER_STRENGTH_MSK 0x0000000CU
#define QCOM_PINMUX_DRIVER_STRENGTH(x)                                                             \
	(((x) << QCOM_PINMUX_DRIVER_STRENGTH_POS) & QCOM_PINMUX_DRIVER_STRENGTH_MSK)

/* Pin modes */
#define QCOM_PIN_MODE_INPUT      0U
#define QCOM_PIN_MODE_OUTPUT     1U
#define QCOM_PIN_MODE_PERIPHERAL 2U

/* GPIO pin numbers for QCC730 (0-14) */
#define QCC730_GPIO_0  0U
#define QCC730_GPIO_1  1U
#define QCC730_GPIO_2  2U
#define QCC730_GPIO_3  3U
#define QCC730_GPIO_4  4U
#define QCC730_GPIO_5  5U
#define QCC730_GPIO_6  6U
#define QCC730_GPIO_7  7U
#define QCC730_GPIO_8  8U
#define QCC730_GPIO_9  9U
#define QCC730_GPIO_10 10U
#define QCC730_GPIO_11 11U
#define QCC730_GPIO_12 12U
#define QCC730_GPIO_13 13U
#define QCC730_GPIO_14 14U

/* Function selections */
#define QCC730_FUNC_GPIO 0U
#define QCC730_FUNC_SPI  1U
#define QCC730_FUNC_UART 2U
#define QCC730_FUNC_I2C  3U
#define QCC730_FUNC_QSPI 5U

/* UART Option values */
#define QCC730_UART_OPTION_0 0U /* GPIO11(TX), GPIO12(RX) */
#define QCC730_UART_OPTION_1 1U /* GPIO14(TX), GPIO13(RX) */
#define QCC730_UART_OPTION_2 2U /* GPIO9(TX), GPIO10(RX) */
#define QCC730_UART_OPTION_3 3U /* GPIO3(TX), GPIO1(RX) */

/* Helper macros for creating pinmux configurations */
#define QCC730_PINMUX(pin, func, mode)                                                             \
	(QCOM_PINMUX_PIN_NUM(pin) | QCOM_PINMUX_FUNC_SEL(func) | QCOM_PINMUX_MODE(mode))

#define QCC730_PINMUX_GPIO_INPUT(pin) QCC730_PINMUX(pin, QCC730_FUNC_GPIO, QCOM_PIN_MODE_INPUT)

#define QCC730_PINMUX_GPIO_OUTPUT(pin) QCC730_PINMUX(pin, QCC730_FUNC_GPIO, QCOM_PIN_MODE_OUTPUT)

#define QCC730_PINMUX_PERIPHERAL(pin, func) QCC730_PINMUX(pin, func, QCOM_PIN_MODE_PERIPHERAL)

/* Enhanced UART pinmux macro with option */
#define QCC730_PINMUX_UART(pin, func, mode, uart_opt)                                              \
	(QCOM_PINMUX_PIN_NUM(pin) | QCOM_PINMUX_FUNC_SEL(func) | QCOM_PINMUX_MODE(mode) |          \
	 QCOM_PINMUX_UART_OPT(uart_opt))

/* UART peripheral with option */
#define QCC730_PINMUX_UART_PERIPHERAL(pin, uart_opt)                                               \
	QCC730_PINMUX_UART(pin, QCC730_FUNC_UART, QCOM_PIN_MODE_PERIPHERAL, uart_opt)

/* Helper macros to extract bit fields from pinctrl_soc_pin_t */
#define QCOM_PINMUX_GET_PIN_NUM(pin_cfg)                                                           \
	(((pin_cfg) & QCOM_PINMUX_PIN_NUM_MSK) >> QCOM_PINMUX_PIN_NUM_POS)

#define QCOM_PINMUX_GET_MODE(pin_cfg) (((pin_cfg) & QCOM_PINMUX_MODE_MSK) >> QCOM_PINMUX_MODE_POS)

#define QCOM_PINMUX_GET_FUNC_SEL(pin_cfg)                                                          \
	(((pin_cfg) & QCOM_PINMUX_FUNC_SEL_MSK) >> QCOM_PINMUX_FUNC_SEL_POS)

#define QCOM_PINMUX_GET_UART_OPT(pin_cfg)                                                          \
	(((pin_cfg) & QCOM_PINMUX_UART_OPT_MSK) >> QCOM_PINMUX_UART_OPT_POS)

#define QCOM_PINMUX_GET_OE(pin_cfg) (((pin_cfg) & QCOM_PINMUX_OE_MSK) >> QCOM_PINMUX_OE_POS)

#define QCOM_PINMUX_GET_PULL_DOWN(pin_cfg)                                                         \
	(((pin_cfg) & QCOM_PINMUX_PULL_DOWN_MSK) >> QCOM_PINMUX_PULL_DOWN_POS)

#define QCOM_PINMUX_GET_PULL_UP(pin_cfg)                                                           \
	(((pin_cfg) & QCOM_PINMUX_PULL_UP_MSK) >> QCOM_PINMUX_PULL_UP_POS)

#define QCOM_PINMUX_GET_DRIVER_STRENGTH(pin_cfg)                                                   \
	(((pin_cfg) & QCOM_PINMUX_DRIVER_STRENGTH_MSK) >> QCOM_PINMUX_DRIVER_STRENGTH_POS)


#endif // ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_QCC730_COMMON_PINCTRL_H_