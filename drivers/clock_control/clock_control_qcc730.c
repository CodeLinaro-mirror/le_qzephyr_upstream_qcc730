/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qcom_qcc730_clkctrl

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/dt-bindings/clock/qcom-qcc730-clock.h>
#include <zephyr/logging/log.h>

#include <soc.h>
#include "qcc730v2.h"

#define LOG_LEVEL CONFIG_CLOCK_CONTROL_LOG_LEVEL

LOG_MODULE_REGISTER(clock_control_qcc730);

/**
 * Clock status after reset value is 0x003BE011. Page 16 of HW Programming Guide.
 * PMU_ROOT_CLK_ENABLE Register Reset Status (0x003BE011)
 *
 * +-----+----------------------------------+--------+
 * | Bit | Field Name                       | Status |
 * +-----+----------------------------------+--------+
 * |  0  | CRCM_CCU_ROOT_CLK_ENABLE         | ON     |
 * |  4  | PSS_GAS2APB_ROOT_CLK_ENABLE      | ON     |
 * |  6  | QTIMER_XO_ROOT_CLK_ENABLE        | OFF    |
 * |  7  | QTIMER_AHB_ROOT_CLK_ENABLE       | OFF    |
 * | 11  | UART_ROOT_CLK_ENABLE             | ON     |
 * | 12  | I2C_ROOT_CLK_ENABLE              | ON     |
 * | 13  | JTAG2AHB_ROOT_CLK_ENABLE         | ON     |
 * | 14  | SIF_AHB_ROOT_CLK_ENABLE          | ON     |
 * | 15  | SIF_SLP_ROOT_CLK_ENABLE          | ON     |
 * | 16  | SIF_SDIOC_ROOT_CLK_ENABLE        | ON     |
 * | 17  | GLB_TMR_XO_ROOT_CLK_ENABLE       | ON     |
 * | 18  | WDOG_XO_ROOT_CLK_ENABLE          | ON     |
 * | 19  | GPIO_ROOT_CLK_ENABLE             | ON     |
 * | 20  | CPR_AHB_ROOT_CLK_ENABLE          | OFF    |
 * | 21  | CPR_XO_ROOT_CLK_ENABLE           | ON     |
 * | 22  | SPI_ROOT_CLK_ENABLE              | OFF    |
 * +-----+----------------------------------+--------+
 * 
 * This clock control driver allows controlling clocks for 
 * UART, I2C, SPI, QTIMER, WDOG and GPIO
 *
 */

struct clock_control_qcc730_config {
	PMU_BASE_pmu_Type *pmu;
};

/* Forward declaration */
static enum clock_control_status clock_control_qcc730_get_status(const struct device *dev,
								 clock_control_subsys_t subsys);

static int clock_control_qcc730_on(const struct device *dev, clock_control_subsys_t subsys)
{
	const struct clock_control_qcc730_config *config = dev->config;
	PMU_BASE_pmu_Type *pmu = config->pmu;

	/* Check if clock is already enabled */
	if (clock_control_qcc730_get_status(dev, subsys) == CLOCK_CONTROL_STATUS_ON) {
		return -EALREADY;
	}

	switch ((uint32_t)subsys) {

	case QCC730_CLOCK_UART:
		pmu->PMU_ROOT_CLK_ENABLE.bit.UART_ROOT_CLK_ENABLE = 1;
		break;
	case QCC730_CLOCK_I2C:
		pmu->PMU_ROOT_CLK_ENABLE.bit.I2C_ROOT_CLK_ENABLE = 1;
		break;
	case QCC730_CLOCK_SPI:
		pmu->PMU_ROOT_CLK_ENABLE.bit.SPI_ROOT_CLK_ENABLE = 1;
		break;
	case QCC730_CLOCK_QTIMER:
		pmu->PMU_ROOT_CLK_ENABLE.bit.QTIMER_XO_ROOT_CLK_ENABLE = 1;
		pmu->PMU_ROOT_CLK_ENABLE.bit.QTIMER_AHB_ROOT_CLK_ENABLE = 1;
		break;
	case QCC730_CLOCK_WDOG:
		pmu->PMU_ROOT_CLK_ENABLE.bit.WDOG_XO_ROOT_CLK_ENABLE = 1;
		pmu->PMU_AON_TOP_CFG.bit.AON_WDOG_SLP_ROOT_CLK_ENABLE = 1;
		break;
	case QCC730_CLOCK_GPIO:
		pmu->PMU_ROOT_CLK_ENABLE.bit.GPIO_ROOT_CLK_ENABLE = 1;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int clock_control_qcc730_off(const struct device *dev, clock_control_subsys_t subsys)
{
	const struct clock_control_qcc730_config *config = dev->config;
	PMU_BASE_pmu_Type *pmu = config->pmu;

	switch ((uint32_t)subsys) {
	case QCC730_CLOCK_UART:
		pmu->PMU_ROOT_CLK_ENABLE.bit.UART_ROOT_CLK_ENABLE = 0;
		break;
	case QCC730_CLOCK_I2C:
		pmu->PMU_ROOT_CLK_ENABLE.bit.I2C_ROOT_CLK_ENABLE = 0;
		break;
	case QCC730_CLOCK_SPI:
		pmu->PMU_ROOT_CLK_ENABLE.bit.SPI_ROOT_CLK_ENABLE = 0;
		break;
	case QCC730_CLOCK_QTIMER:
		pmu->PMU_ROOT_CLK_ENABLE.bit.QTIMER_XO_ROOT_CLK_ENABLE = 0;
		pmu->PMU_ROOT_CLK_ENABLE.bit.QTIMER_AHB_ROOT_CLK_ENABLE = 0;
		break;
	case QCC730_CLOCK_WDOG:
		pmu->PMU_ROOT_CLK_ENABLE.bit.WDOG_XO_ROOT_CLK_ENABLE = 0;
		pmu->PMU_AON_TOP_CFG.bit.AON_WDOG_SLP_ROOT_CLK_ENABLE = 0;
		break;
	case QCC730_CLOCK_GPIO:
		pmu->PMU_ROOT_CLK_ENABLE.bit.GPIO_ROOT_CLK_ENABLE = 0;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static enum clock_control_status clock_control_qcc730_get_status(const struct device *dev,
								 clock_control_subsys_t subsys)
{
	const struct clock_control_qcc730_config *config = dev->config;
	PMU_BASE_pmu_Type *pmu = config->pmu;
	bool enabled = false;

	switch ((uint32_t)subsys) {

	case QCC730_CLOCK_UART:
		enabled = pmu->PMU_ROOT_CLK_ENABLE.bit.UART_ROOT_CLK_ENABLE;
		break;
	case QCC730_CLOCK_I2C:
		enabled = pmu->PMU_ROOT_CLK_ENABLE.bit.I2C_ROOT_CLK_ENABLE;
		break;
	case QCC730_CLOCK_SPI:
		enabled = pmu->PMU_ROOT_CLK_ENABLE.bit.SPI_ROOT_CLK_ENABLE;
		break;
	case QCC730_CLOCK_QTIMER:
		enabled = pmu->PMU_ROOT_CLK_ENABLE.bit.QTIMER_XO_ROOT_CLK_ENABLE &&
			  pmu->PMU_ROOT_CLK_ENABLE.bit.QTIMER_AHB_ROOT_CLK_ENABLE;
		break;
	case QCC730_CLOCK_WDOG:
		enabled = pmu->PMU_ROOT_CLK_ENABLE.bit.WDOG_XO_ROOT_CLK_ENABLE &&
			  pmu->PMU_AON_TOP_CFG.bit.AON_WDOG_SLP_ROOT_CLK_ENABLE;
		break;
	case QCC730_CLOCK_GPIO:
		enabled = pmu->PMU_ROOT_CLK_ENABLE.bit.GPIO_ROOT_CLK_ENABLE;
		break;
	default:
		return CLOCK_CONTROL_STATUS_UNKNOWN;
	}
	return enabled ? CLOCK_CONTROL_STATUS_ON : CLOCK_CONTROL_STATUS_OFF;
}

static const struct clock_control_driver_api clock_control_qcc730_api = {
	.on = clock_control_qcc730_on,
	.off = clock_control_qcc730_off,
	.get_status = clock_control_qcc730_get_status,
};

static const struct clock_control_qcc730_config clock_control_qcc730_cfg = {
	.pmu = (PMU_BASE_pmu_Type *)DT_REG_ADDR(DT_NODELABEL(pmu)),
};

DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, &clock_control_qcc730_cfg, PRE_KERNEL_1,
		      CONFIG_CLOCK_CONTROL_INIT_PRIORITY, &clock_control_qcc730_api);
