/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qcom_qcc730_reset

#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/logging/log.h>
#include <zephyr/dt-bindings/reset/qcom_qcc730_reset.h>
#include "soc.h"

#define QCC730_RESET_REG(id) ((id) >> 5U)
#define QCC730_RESET_BIT(id) ((id) & 0x1F)

LOG_MODULE_REGISTER(reset_qcc730, CONFIG_RESET_LOG_LEVEL);

struct reset_qcc730_config {
	PMU_BASE_pmu_Type *pmu;
	MCU_BASE_mcu_Type *mcu;
};

static int reset_qcc730_status(const struct device *dev, uint32_t id, uint8_t *status)
{
	const struct reset_qcc730_config *config = dev->config;

	if (QCC730_RESET_REG(id) == QCC730_PMU_SOFT_RESET) {
		*status = (config->pmu->PMU_SOFT_RESET.reg & BIT(QCC730_RESET_BIT(id))) ? 1U : 0U;
	} else if (QCC730_RESET_REG(id) == QCC730_MCU_SOFT_RESET) {
		*status = (config->mcu->MCU_SOFT_RESET.reg & BIT(QCC730_RESET_BIT(id))) ? 1U : 0U;
	} else {
		LOG_ERR("Invalid reset ID %u", id);
		return -EINVAL;
	}

	return 0;
}

static int reset_qcc730_line_assert(const struct device *dev, uint32_t id)
{
	const struct reset_qcc730_config *config = dev->config;

	if (QCC730_RESET_REG(id) == QCC730_PMU_SOFT_RESET) {
		config->pmu->PMU_SOFT_RESET.reg |= BIT(QCC730_RESET_BIT(id));
	} else if (QCC730_RESET_REG(id) == QCC730_MCU_SOFT_RESET) {
		config->mcu->MCU_SOFT_RESET.reg |= BIT(QCC730_RESET_BIT(id));
	} else {
		LOG_ERR("Invalid reset ID %u", id);
		return -EINVAL;
	}

	return 0;
}

static int reset_qcc730_line_deassert(const struct device *dev, uint32_t id)
{
	const struct reset_qcc730_config *config = dev->config;

	if (QCC730_RESET_REG(id) == QCC730_PMU_SOFT_RESET) {
		config->pmu->PMU_SOFT_RESET.reg &= ~(BIT(QCC730_RESET_BIT(id)));
	} else if (QCC730_RESET_REG(id) == QCC730_MCU_SOFT_RESET) {
		config->mcu->MCU_SOFT_RESET.reg &= ~(BIT(QCC730_RESET_BIT(id)));
	} else {
		LOG_ERR("Invalid reset ID %u", id);
		return -EINVAL;
	}

	return 0;
}

static int reset_qcc730_line_toggle(const struct device *dev, uint32_t id)
{
	int ret = 0;

	ret = reset_qcc730_line_assert(dev, id);
	if (ret < 0) {
		return ret;
	}

	ret = reset_qcc730_line_deassert(dev, id);

	return ret;
}

static DEVICE_API(reset, reset_qcc730_driver_api) = {
	.status = reset_qcc730_status,
	.line_assert = reset_qcc730_line_assert,
	.line_deassert = reset_qcc730_line_deassert,
	.line_toggle = reset_qcc730_line_toggle,
};

#define RESET_QCC730_INST(inst)                                                                    \
	static const struct reset_qcc730_config reset_qcc730_config_##inst = {                     \
		.pmu = (PMU_BASE_pmu_Type *)DT_INST_REG_ADDR_BY_NAME(inst, pmu_soft_reset),        \
		.mcu = (MCU_BASE_mcu_Type *)DT_INST_REG_ADDR_BY_NAME(inst, mcu_soft_reset),        \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, NULL, NULL, NULL, &reset_qcc730_config_##inst, PRE_KERNEL_1,   \
			      CONFIG_RESET_INIT_PRIORITY, &reset_qcc730_driver_api)

DT_INST_FOREACH_STATUS_OKAY(RESET_QCC730_INST)
