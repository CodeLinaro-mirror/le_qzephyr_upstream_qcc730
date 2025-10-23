/*
 * Copyright (c) 2025 Qualcomm Technologies, Inc. and/or its subsidiaries
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_ARM_QCOM_COMMON_PINCTRL_SOC_H_
#define ZEPHYR_SOC_ARM_QCOM_COMMON_PINCTRL_SOC_H_

#include <zephyr/devicetree.h>
#include <zephyr/types.h>
#include <zephyr/dt-bindings/pinctrl/qcom-qcc730-pinctrl.h>

/**
 * @brief Initializes a single pin by combining its base value with common
 * properties from its parent devicetree group node.
 *
 * Bias handling:
 * - If bias-disable is present, both pull-up and pull-down are forced to 0
 * - Otherwise, bias-pull-up and bias-pull-down are read from DT properties
 *
 * Drive strength (bits [3:2]):
 * - high-drive-strength present (1) -> value 1 (high drive)
 * - high-drive-strength absent (0) -> value 0 (low drive)
 */

#define Z_PINCTRL_STATE_PIN_INIT(node_id, prop, idx)                                               \
	(DT_PROP(node_id, bias_disable) ? /* clear both pull bits when bias-disable is set */      \
		 (((DT_PROP_BY_IDX(node_id, prop, idx)) &                                          \
		   ~(QCOM_PINMUX_PULL_UP_MSK | QCOM_PINMUX_PULL_DOWN_MSK)) |                       \
		  (DT_PROP(node_id, high_drive_strength) << QCOM_PINMUX_DRIVER_STRENGTH_POS))      \
					: /* otherwise, set pulls based on DT properties */        \
		 ((DT_PROP_BY_IDX(node_id, prop, idx)) |                                           \
		  (DT_PROP(node_id, bias_pull_up) << QCOM_PINMUX_PULL_UP_POS) |                    \
		  (DT_PROP(node_id, bias_pull_down) << QCOM_PINMUX_PULL_DOWN_POS) |                \
		  (DT_PROP(node_id, high_drive_strength) << QCOM_PINMUX_DRIVER_STRENGTH_POS))),

/**
 * @brief Initializes all pins for a pinctrl state by iterating through its groups.
 *
 * This is the main macro which initializes the pinctrl_soc_pin_t array for the pins
 *
 */
#define Z_PINCTRL_STATE_PINS_INIT(node_id, prop)                                                   \
	{                                                                                          \
		DT_FOREACH_CHILD_VARGS(DT_PHANDLE(node_id, prop), DT_FOREACH_PROP_ELEM, pinmux,    \
				       Z_PINCTRL_STATE_PIN_INIT)                                   \
	}

/**
 * @brief QCC730 Pin Control Definitions
 */

/**
 * @brief QCC730 pincfg bit field.
 *
 * It's a 32-bit value that encodes all pin configuration information.
 *
 * Fields:
 *
 * - 24..31: pin number (0-14)
 * - 22..23: reserved
 * - 20..21: reserved
 * - 18..19: reserved
 * - 16..17: UART option (0-3) - for UART function pins only
 * - 14..15: mode (input/output/periph)
 * -  8..13: function selection
 * -      7: reserved
 * -      6: GPIO Output Enable
 * -      5: Pull Down (1=enabled, 0=disabled)
 * -      4: Pull Up (1=enabled, 0=disabled)
 * -   2..3: Driver Strength (0=low drive, 1=high drive)
 * -   0..1: reserved
 */
typedef uint32_t pinctrl_soc_pin_t;

#endif /* ZEPHYR_SOC_ARM_QCOM_COMMON_PINCTRL_SOC_H_ */
