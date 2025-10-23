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
#include <zephyr/pm/device.h>

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

struct wdt_qcc730_data {
	bool wdt_initialized;
};

static int wdt_qcc730_setup(const struct device *dev, uint8_t options)
{
	const struct wdt_qcc730_cfg *wdt_cfg = dev->config;
	struct wdt_qcc730_data *data = dev->data;
	PMU_BASE_pmu_Type *pmu_regs = wdt_cfg->pmu;

	/* Error while suspended/uninitialized */
	if (!data->wdt_initialized) {
		LOG_ERR("Watchdog in sleep state or not initialized!");
		return -EBUSY;
	}

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
	struct wdt_qcc730_data *data = dev->data;
	PMU_BASE_pmu_Type *pmu_regs = wdt_cfg->pmu;

	/* Error while suspended/uninitialized */
	if (!data->wdt_initialized) {
		LOG_ERR("Watchdog in sleep state or not initialized!");
		return -EBUSY;
	}

	pmu_regs->PMU_WDOG_CTL.bit.WDOG_ENABLE = 0U;
	pmu_regs->PMU_AON_WDOG_CTL.bit.WDOG_ENABLE = 0U;

	return 0;
}

static int wdt_qcc730_install_timeout(const struct device *dev, const struct wdt_timeout_cfg *cfg)
{
	const struct wdt_qcc730_cfg *wdt_cfg = dev->config;
	struct wdt_qcc730_data *data = dev->data;
	PMU_BASE_pmu_Type *pmu_regs = wdt_cfg->pmu;

	/* Error while suspended/uninitialized */
	if (!data->wdt_initialized) {
		LOG_ERR("Watchdog in sleep state or not initialized!");
		return -EBUSY;
	}

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
	struct wdt_qcc730_data *data = dev->data;
	PMU_BASE_pmu_Type *pmu_regs = wdt_cfg->pmu;

	/* Error while suspended/uninitialized */
	if (!data->wdt_initialized) {
		return -EBUSY;
	}

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

#ifdef CONFIG_PM_DEVICE
/* Platform enable/disable, used by PM functions. */
static int wdt_qcc730_platform(const struct device *dev, uint8_t enable)
{
	const struct wdt_qcc730_cfg *wdt_cfg = dev->config;
	struct wdt_qcc730_data *data = dev->data;
	int ret = 0;

	if (wdt_cfg->clock_dev) {
		if(enable) {
			if (!device_is_ready(wdt_cfg->clock_dev)) {
				return -ENODEV;
			}
			ret = clock_control_on(wdt_cfg->clock_dev, wdt_cfg->clock_subsys);
			if (ret < 0 && ret != -EALREADY) {
				LOG_ERR("Error turning watchdog clock on for sleep (%d)", ret);
				return ret;
			}
			data->wdt_initialized = true;
		} else {
			ret = clock_control_off(wdt_cfg->clock_dev, wdt_cfg->clock_subsys);
			if (ret < 0) {
				LOG_ERR("Error turning watchdog clock off for sleep (%d)", ret);
				return ret;
			}
			data->wdt_initialized = false;
		}
	} else {
		LOG_ERR("No clock device defined for watchdog");
		ret = -ENODEV;
	}

	return ret;
}

static int wdt_qcc730_enable(const struct device *dev)
{
	return wdt_qcc730_platform(dev, 1U);
}

static int wdt_qcc730_suspend(const struct device *dev)
{
	return wdt_qcc730_platform(dev, 0U);
}

static int wdt_qcc730_pm_action(const struct device *dev, enum pm_device_action action)
{
	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		return wdt_qcc730_enable(dev);
	case PM_DEVICE_ACTION_SUSPEND:
		return wdt_qcc730_suspend(dev);
	default:
		return -ENOTSUP;
	}
}
#endif // CONFIG_PM_DEVICE

static int wdt_qcc730_init(const struct device *dev)
{
	const struct wdt_qcc730_cfg *wdt_cfg = dev->config;
	struct wdt_qcc730_data *data = dev->data;
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

	/* Mark initialized */
	data->wdt_initialized = true;

	return ret;
}

#define WDT_QCC730_INIT(n)                                                                         \
	static const struct wdt_qcc730_cfg wdt_qcc730_config_##n = {                               \
		.pmu = (PMU_BASE_pmu_Type *)DT_REG_ADDR(DT_NODELABEL(pmu)),                        \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, id),                \
	};                                                                                         \
                                                                                                   \
	static struct wdt_qcc730_data wdt_qcc730_data_##n = {                                      \
		.wdt_initialized = false,                                                          \
	};                                                                                         \
                                                                                                   \
	PM_DEVICE_DT_INST_DEFINE(n, wdt_qcc730_pm_action);                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, wdt_qcc730_init, PM_DEVICE_DT_INST_GET(n),                        \
			      &wdt_qcc730_data_##n, &wdt_qcc730_config_##n,                        \
			      PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &wdt_qcc730_api);

DT_INST_FOREACH_STATUS_OKAY(WDT_QCC730_INIT)
