/*
 * Copyright (c) 2025 Qualcomm Technologies, Inc. and/or its subsidiaries
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qcom_qcc730_pinctrl

#include <zephyr/device.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/qcom-qcc730-pinctrl.h>

#include "soc.h"

LOG_MODULE_REGISTER(pinctrl_qcc730, CONFIG_PINCTRL_LOG_LEVEL);

#define QCC730_PMU_BOOT_STRAP_UNLOCK (0x63887466)

/* Get PMU base address from device tree */
#define PMU_BASE_ADDR ((PMU_BASE_pmu_Type *)DT_REG_ADDR(DT_NODELABEL(pmu)))

static int pinctrl_qcc730_pin_configure(pinctrl_soc_pin_t pin_cfg)
{
	PMU_BASE_pmu_Type *pmu = PMU_BASE_ADDR;
	/* Extract pin configuration fields */
	uint32_t pin_num = QCOM_PINMUX_GET_PIN_NUM(pin_cfg);
	uint32_t mode = QCOM_PINMUX_GET_MODE(pin_cfg);
	uint32_t func_sel = QCOM_PINMUX_GET_FUNC_SEL(pin_cfg);
	uint8_t uart_opt = QCOM_PINMUX_GET_UART_OPT(pin_cfg);
	uint32_t pull_up = QCOM_PINMUX_GET_PULL_UP(pin_cfg);
	uint32_t pull_down = QCOM_PINMUX_GET_PULL_DOWN(pin_cfg);
	uint32_t drive_strength = QCOM_PINMUX_GET_DRIVER_STRENGTH(pin_cfg);

	ARG_UNUSED(mode);

	/* Validate pin number */
	if (pin_num > QCC730_GPIO_14) {
		return -EINVAL;
	}
	pmu->PMU_BOOT_STRAP_CONFIG_SECURE.reg = QCC730_PMU_BOOT_STRAP_UNLOCK;
	/* Configure pin multiplexing function */
	if (func_sel != QCC730_FUNC_GPIO) {
		/* Set peripheral function */
		switch (func_sel) {
		case QCC730_FUNC_UART:
			/* Configure UART pins with pull-up enabled, pull-down disabled */
			pmu->PMU_BOOT_STRAP_CONFIGURATION_STATUS.bit.CFG_UART_OPTION = uart_opt;
			break;

		case QCC730_FUNC_SPI:
			return -ENOTSUP;
			break;

		case QCC730_FUNC_I2C:
			return -ENOTSUP;
			break;

		case QCC730_FUNC_QSPI:
			return -ENOTSUP;
			break;
		default:
			break;
		}
	}

	/* Configure pull-up resistor using PMU_CFG_IOPAD_PU */
	/* READ-MODIFY-WRITE */
	uint32_t pu_reg = pmu->PMU_CFG_IOPAD_PU.reg;
	WRITE_BIT(pu_reg, pin_num, pull_up);
	pmu->PMU_CFG_IOPAD_PU.reg = pu_reg;

	/* Configure pull-down resistor using PMU_CFG_IOPAD_PD */
	/* READ-MODIFY-WRITE */
	uint32_t pd_reg = pmu->PMU_CFG_IOPAD_PD.reg;
	WRITE_BIT(pd_reg, pin_num, pull_down);
	pmu->PMU_CFG_IOPAD_PD.reg = pd_reg;

	/* Configure drive strength using PMU_CFG_IOPAD_DS */
	/* READ-MODIFY-WRITE */
	uint32_t ds_reg = pmu->PMU_CFG_IOPAD_DS.reg;
	WRITE_BIT(ds_reg, pin_num, drive_strength);
	pmu->PMU_CFG_IOPAD_DS.reg = ds_reg;

	return 0;
}

int pinctrl_configure_pins(const pinctrl_soc_pin_t *pins, uint8_t pin_cnt, uintptr_t reg)
{
	int ret;

	ARG_UNUSED(reg);

	for (uint8_t i = 0; i < pin_cnt; i++) {
		ret = pinctrl_qcc730_pin_configure(pins[i]);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}
