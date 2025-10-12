/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qcom_qcc730_wdt

#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/watchdog.h>

#include "soc.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wdt_qcc730, CONFIG_WDT_LOG_LEVEL);

#define QCC730_PMU_WDOG_MAX_TIMEOUT     BIT_MASK(21)
#define QCC730_PMU_WDOG_LOAD_SECURE_VAL 0xA1A602E7

struct wdt_qcc730_cfg {
	PMU_BASE_pmu_Type *pmu;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
};

static int wdt_qcc730_setup(const struct device *dev, uint8_t options)
{
	const struct wdt_qcc730_cfg *wdt_cfg = dev->config;
	PMU_BASE_pmu_Type *pmu_regs = wdt_cfg->pmu;

	if (options & WDT_OPT_PAUSE_IN_SLEEP) {
		LOG_ERR("Pause in sleep not supported");
		return -ENOTSUP;
	}

	if (options & WDT_OPT_PAUSE_HALTED_BY_DBG) {
		LOG_ERR("Pause by debugger not supported");
		return -ENOTSUP;
	}

	// Enable the AON watchdog function
	pmu_regs->PMU_AON_WDOG_CTL.bit.WDOG_ENABLE = 1U;

	// Reset the watchdog timers
	pmu_regs->PMU_WDOG_CTL.bit.WDOG_RESET = 1U;
	pmu_regs->PMU_WDOG_CTL.bit.WDOG_RESET = 0U;
	pmu_regs->PMU_AON_WDOG_CTL.bit.WDOG_RESET = 1U;
	pmu_regs->PMU_AON_WDOG_CTL.bit.WDOG_RESET = 0U;

	// Unfreeze the watchdog timers
	pmu_regs->PMU_WDOG_CTL.bit.WDOG_FREEZE = 0U;
	pmu_regs->PMU_AON_WDOG_CTL.bit.WDOG_FREEZE = 0U;

	return 0;
}

static int wdt_qcc730_disable(const struct device *dev)
{
	const struct wdt_qcc730_cfg *wdt_cfg = dev->config;
	PMU_BASE_pmu_Type *pmu_regs = wdt_cfg->pmu;

	pmu_regs->PMU_WDOG_CTL.bit.WDOG_ENABLE = 0U;
	pmu_regs->PMU_AON_WDOG_CTL.bit.WDOG_ENABLE = 0U;

	return 0;
}

static int wdt_qcc730_install_timeout(const struct device *dev, const struct wdt_timeout_cfg *cfg)
{
	const struct wdt_qcc730_cfg *wdt_cfg = dev->config;
	PMU_BASE_pmu_Type *pmu_regs = wdt_cfg->pmu;

	if (cfg == NULL || cfg->window.min > 0) {
		LOG_ERR("Wrong timeout configuration");
		return -EINVAL;
	}

	if (cfg->window.max == 0 || cfg->window.max > QCC730_PMU_WDOG_MAX_TIMEOUT) {
		LOG_ERR("Upper limit timeout out of range");
		return -EINVAL;
	}

	if (cfg->flags != WDT_FLAG_RESET_SOC) {
		LOG_ERR("Only SoC reset supported");
		return -ENOTSUP;
	}

	// Bark timeout is not supported by the hardware. It has to be set to the same value as bite
	// timeout.
	// Wait for the watchdog bark time register to be ready
	while (pmu_regs->PMU_WDOG_BARK_TIME.bit.SYNC_STATUS)
		;
	// Load the watchdog bark timeout
	pmu_regs->PMU_WDOG_BARK_TIME.bit.WDOG_BARK_TIME = cfg->window.max;

	// Wait for the AON watchdog bark time register to be ready
	while (pmu_regs->PMU_AON_WDOG_BARK_TIME.bit.SYNC_STATUS)
		;
	// Load the AON watchdog bark timeout
	pmu_regs->PMU_AON_WDOG_BARK_TIME.bit.WDOG_BARK_TIME = cfg->window.max;

	// Unlock the watchdog bite secure register
	pmu_regs->PMU_WDOG_BITE_SECURE.reg = QCC730_PMU_WDOG_LOAD_SECURE_VAL;
	// Wait for the bite time register to be ready
	while (pmu_regs->PMU_WDOG_BITE_TIME.bit.SYNC_STATUS)
		;
	// Load the watchdog bite timeout
	pmu_regs->PMU_WDOG_BITE_TIME.bit.WDOG_BITE_TIME = cfg->window.max;

	// Unlock the AON watchdog bite secure register
	pmu_regs->PMU_AON_WDOG_BITE_SECURE.reg = QCC730_PMU_WDOG_LOAD_SECURE_VAL;
	// Wait for the bite time register to be ready
	while (pmu_regs->PMU_AON_WDOG_BITE_TIME.bit.SYNC_STATUS)
		;
	// Load the AON watchdog bite timeout
	pmu_regs->PMU_AON_WDOG_BITE_TIME.bit.WDOG_BITE_TIME = cfg->window.max;

	return 0;
}

static int wdt_qcc730_feed(const struct device *dev, int channel_id)
{
	ARG_UNUSED(channel_id);
	const struct wdt_qcc730_cfg *wdt_cfg = dev->config;
	PMU_BASE_pmu_Type *pmu_regs = wdt_cfg->pmu;

	pmu_regs->PMU_AON_WDOG_CTL.bit.WDOG_RESET = 1U;
	pmu_regs->PMU_AON_WDOG_CTL.bit.WDOG_RESET = 0U;

	return 0;
}

static DEVICE_API(wdt, wdt_qcc730_api) = {
	.setup = wdt_qcc730_setup,
	.disable = wdt_qcc730_disable,
	.install_timeout = wdt_qcc730_install_timeout,
	.feed = wdt_qcc730_feed,
};

static int wdt_qcc730_init(const struct device *dev)
{
	const struct wdt_qcc730_cfg *wdt_cfg = dev->config;
	PMU_BASE_pmu_Type *pmu_regs = wdt_cfg->pmu;
	int ret = 0;

	// Enable root clock of watchdog
	if (wdt_cfg->clock_dev) {
		if (!device_is_ready(wdt_cfg->clock_dev)) {
			return -ENODEV;
		}
		ret = clock_control_on(wdt_cfg->clock_dev, wdt_cfg->clock_subsys);
		// Skip -EALREADY error if clock is already enabled
		if (ret < 0 && ret != -EALREADY) {
			return ret;
		}
	}

	return ret;
}

#define WDT_QCC730_INIT(n)                                                                         \
	static const struct wdt_qcc730_cfg wdt_qcc730_config_##n = {                               \
		.pmu = (PMU_BASE_pmu_Type *)DT_REG_ADDR(DT_NODELABEL(pmu)),                        \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, id),                \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, wdt_qcc730_init, NULL, NULL, &wdt_qcc730_config_##n,              \
			      PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &wdt_qcc730_api);

DT_INST_FOREACH_STATUS_OKAY(WDT_QCC730_INIT)
