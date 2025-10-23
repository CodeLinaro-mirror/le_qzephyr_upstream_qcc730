/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qcom_qcc730_qtmr

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/pm/device.h>
#include "soc.h"

#define QCC730_QTMR_AC_CNTACR_Mask 0x3F
#define QCC730_QTMR_MAX_VAL        BIT64_MASK(56)

LOG_MODULE_REGISTER(qtmr_qcc730, CONFIG_COUNTER_LOG_LEVEL);

struct qtmr_qcc730_cfg {
	struct counter_config_info info;
	int frame_id;
	QTMR_AC_BASE_qtmr_ac_Type *access_control_regs;
	QTMR_V1_T0_BASE_qtmr_v1_t0_Type *qtmr_regs;
	PMU_BASE_pmu_Type *pmu_regs;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	const int irqn;
};

struct qtmr_qcc730_data {
	counter_alarm_callback_t callback;
	void *user_data;
	uint64_t ticks;
#ifdef CONFIG_PM_DEVICE
	bool qtmr_initialized;
#endif
};

static int qtmr_qcc730_start(const struct device *dev)
{
	const struct qtmr_qcc730_cfg *const cfg = dev->config;
#ifdef CONFIG_PM_DEVICE
	struct qtmr_qcc730_data *const data = dev->data;

	if (!data->qtmr_initialized) {
		LOG_ERR("Qtimer is suspended/not initialized");
		return -EBUSY;
	}
#endif

	// Enable the timer
	cfg->qtmr_regs->QTMR_V1_CNTP_CTL.bit.EN = 1U;
	// Mask the interrupt; it will be enabled when an alarm is set
	cfg->qtmr_regs->QTMR_V1_CNTP_CTL.bit.IMSK = 1U;

	return 0;
}

static int qtmr_qcc730_stop(const struct device *dev)
{
	const struct qtmr_qcc730_cfg *const cfg = dev->config;
#ifdef CONFIG_PM_DEVICE
	struct qtmr_qcc730_data *const data = dev->data;

	if (!data->qtmr_initialized) {
		LOG_ERR("Qtimer is suspended/not initialized");
		return -EBUSY;
	}
#endif
	// Disable the timer
	cfg->qtmr_regs->QTMR_V1_CNTP_CTL.bit.EN = 0U;
	cfg->qtmr_regs->QTMR_V1_CNTP_CTL.bit.IMSK = 1U;

	return 0;
}

static int qtmr_qcc730_get_value(const struct device *dev, uint32_t *ticks)
{
	// Qtimer value is 56-bit long
	ARG_UNUSED(dev);
	ARG_UNUSED(ticks);
	return -ENOTSUP;
}

static int qtmr_qcc730_get_value_64(const struct device *dev, uint64_t *ticks)
{
	const struct qtmr_qcc730_cfg *const cfg = dev->config;
	uint32_t count_lo = 0;
	uint32_t count_hi = 0;
#ifdef CONFIG_PM_DEVICE
	struct qtmr_qcc730_data *const data = dev->data;

	if (!data->qtmr_initialized) {
		LOG_ERR("Qtimer is suspended/not initialized");
		return -EBUSY;
	}
#endif
	count_lo = cfg->qtmr_regs->QTMR_V1_CNTPCT_LO.reg;
	count_hi = cfg->qtmr_regs->QTMR_V1_CNTPCT_HI.reg;

	*ticks = (((uint64_t)count_hi << 32) | count_lo);

	return 0;
}

static void qtmr_qcc730_set_cval(const struct device *dev, uint64_t value)
{
	const struct qtmr_qcc730_cfg *const cfg = dev->config;

	cfg->qtmr_regs->QTMR_V1_CNTP_CVAL_LO.reg = (uint32_t)(value & UINT32_MAX);
	cfg->qtmr_regs->QTMR_V1_CNTP_CVAL_HI.reg = (uint32_t)(value >> 32);
}

static int qtmr_qcc730_set_alarm(const struct device *dev, uint8_t chan_id,
				 const struct counter_alarm_cfg *alarm_cfg)
{
	const struct qtmr_qcc730_cfg *const cfg = dev->config;
	struct qtmr_qcc730_data *const data = dev->data;
	uint64_t now = 0UL;
	uint64_t alarm_value = 0UL;

#ifdef CONFIG_PM_DEVICE
	if (!data->qtmr_initialized) {
		LOG_ERR("Qtimer is suspended/not initialized");
		return -EBUSY;
	}
#endif

	if (cfg->qtmr_regs->QTMR_V1_CNTP_CTL.bit.IMSK == 0U) {
		LOG_ERR("Alarm already set for timer frame %u", cfg->frame_id);
		return -EBUSY;
	}

	LOG_DBG("Setting alarm for timer frame %u, ticks %u", cfg->frame_id, alarm_cfg->ticks);

	qtmr_qcc730_get_value_64(dev, &now);

	if (now + alarm_cfg->ticks > QCC730_QTMR_MAX_VAL) {
		LOG_ERR("Alarm time %u exceeds maximum value for timer frame %u", alarm_cfg->ticks,
			cfg->frame_id);
		return -EINVAL;
	}

	if ((alarm_cfg->flags & COUNTER_ALARM_CFG_ABSOLUTE) == 0) {
		alarm_value = now + (uint64_t)alarm_cfg->ticks;
	} else {
		return -ENOTSUP;
	}

	qtmr_qcc730_set_cval(dev, alarm_value);

	data->callback = alarm_cfg->callback;
	data->user_data = alarm_cfg->user_data;
	data->ticks = alarm_cfg->ticks;

	// Enable the interrupt for the timer
	cfg->qtmr_regs->QTMR_V1_CNTP_CTL.bit.IMSK = 0U;

	return 0;
}

static int qtmr_qcc730_cancel_alarm(const struct device *dev, uint8_t chan_id)
{
	const struct qtmr_qcc730_cfg *const cfg = dev->config;
#ifdef CONFIG_PM_DEVICE
	struct qtmr_qcc730_data *const data = dev->data;

	if (!data->qtmr_initialized) {
		LOG_ERR("Qtimer is suspended/not initialized");
		return -EBUSY;
	}
#endif

	// Disable the interrupt for the timer
	cfg->qtmr_regs->QTMR_V1_CNTP_CTL.bit.IMSK = 1U;
	return 0;
}

static uint32_t qtmr_qcc730_get_pending_int(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static uint32_t qtmr_qcc730_get_freq(const struct device *dev)
{
	const struct qtmr_qcc730_cfg *const cfg = dev->config;

	return cfg->info.freq;
}

int qtmr_qcc730_set_alarm_absolute(const struct device *dev, uint8_t chan_id,
				   const struct counter_alarm_cfg *alarm_cfg, uint64_t ticks)
{
	const struct qtmr_qcc730_cfg *const cfg = dev->config;
	struct qtmr_qcc730_data *const data = dev->data;
	uint64_t now = 0UL;

#ifdef CONFIG_PM_DEVICE
	if (!data->qtmr_initialized) {
		LOG_ERR("Qtimer is suspended/not initialized");
		return -EBUSY;
	}
#endif

	if (cfg->qtmr_regs->QTMR_V1_CNTP_CTL.bit.IMSK == 0U) {
		LOG_ERR("Alarm already set for timer frame %u", cfg->frame_id);
		return -EBUSY;
	}

	if (ticks > QCC730_QTMR_MAX_VAL) {
		LOG_ERR("Alarm time %llu exceeds maximum value for timer frame %u", ticks,
			cfg->frame_id);
		return -EINVAL;
	}

	if ((alarm_cfg->flags & COUNTER_ALARM_CFG_ABSOLUTE) == 0) {
		return -EINVAL;
	}

	qtmr_qcc730_get_value_64(dev, &now);
	if (ticks < now) {
		LOG_ERR("Alarm time %llu is in the past for timer frame %u", ticks, cfg->frame_id);
		return -EINVAL;
	}

	LOG_DBG("Setting alarm for timer frame %u, ticks %llu", cfg->frame_id, ticks);

	qtmr_qcc730_set_cval(dev, ticks);

	data->callback = alarm_cfg->callback;
	data->user_data = alarm_cfg->user_data;
	data->ticks = ticks;

	// Enable the interrupt for the timer
	cfg->qtmr_regs->QTMR_V1_CNTP_CTL.bit.IMSK = 0U;

	return 0;
}

static void qtmr_qcc730_isr(const struct device *dev)
{
	const struct qtmr_qcc730_cfg *const cfg = dev->config;
	struct qtmr_qcc730_data *data = dev->data;

	// Mask the interrupt
	cfg->qtmr_regs->QTMR_V1_CNTP_CTL.bit.IMSK = 1U;

	LOG_DBG("Qtimer frame %u ISR triggered", cfg->frame_id);

	if (data->callback != NULL) {
		if (data->ticks > UINT32_MAX) {
			data->ticks = UINT32_MAX;
		}
		data->callback(dev, 0, (uint32_t)data->ticks, data->user_data);
	} else {
		LOG_WRN("Qtimer frame %u ISR: no callback registered", cfg->frame_id);
	}
}

static inline int qtmr_qcc730_frame_init(const struct device *dev)
{
	const struct qtmr_qcc730_cfg *cfg = dev->config;
	enum clock_control_status clk_status = CLOCK_CONTROL_STATUS_OFF;
	int ret = 0;

	// Mask frame interrupt
	cfg->qtmr_regs->QTMR_V1_CNTP_CTL.bit.IMSK = 1U;
	// Enable clocks
	if (cfg->clock_dev) {
		if (!device_is_ready(cfg->clock_dev)) {
			LOG_ERR("Clock device not ready for Qtimer");
			return -ENODEV;
		}
		clk_status = clock_control_get_status(cfg->clock_dev, cfg->clock_subsys);
		if (clk_status != CLOCK_CONTROL_STATUS_ON) {
			ret = clock_control_on(cfg->clock_dev, cfg->clock_subsys);
			if (ret < 0) {
				LOG_ERR("Qtimer frame %u Clock Control ERROR: %d", cfg->frame_id,
					ret);
				return ret;
			}
		}
	}

	cfg->pmu_regs->PMU_SON_GDSCR.bit.RETAIN_FF_ENABLE = 1U;

	// Set counter frequency
	cfg->access_control_regs->QTMR_AC_CNTFRQ.reg = cfg->info.freq;

	// Enable access to CNTP registers
	cfg->access_control_regs->QTMR_AC_CNTACR[cfg->frame_id].reg |= QCC730_QTMR_AC_CNTACR_Mask;

	LOG_DBG("Qtimer frame %u initialized", cfg->frame_id);

	return ret;
}

#ifdef CONFIG_PM_DEVICE
static inline int qtmr_qcc730_frame_deinit(const struct device *dev)
{
	const struct qtmr_qcc730_cfg *cfg = dev->config;
	enum clock_control_status clk_status = CLOCK_CONTROL_STATUS_ON;
	int ret = 0;

	// Mask frame interrupt
	cfg->qtmr_regs->QTMR_V1_CNTP_CTL.bit.IMSK = 1U;
	// Disable clocks
	if (cfg->clock_dev) {
		if (!device_is_ready(cfg->clock_dev)) {
			LOG_ERR("Clock device not ready for Qtimer");
			return -ENODEV;
		}
		clk_status = clock_control_get_status(cfg->clock_dev, cfg->clock_subsys);
		if (clk_status != CLOCK_CONTROL_STATUS_OFF) {
			ret = clock_control_off(cfg->clock_dev, cfg->clock_subsys);
			if (ret < 0) {
				LOG_ERR("Qtimer frame %u Clock Control ERROR: %d", cfg->frame_id,
					ret);
				return ret;
			}
		}
	}

	cfg->pmu_regs->PMU_SON_GDSCR.bit.RETAIN_FF_ENABLE = 0U;

	return ret;
}
#endif /* CONFIG_PM_DEVICE */

static int qtmr_qcc730_init(const struct device *dev)
{
	int ret = 0;
	const struct qtmr_qcc730_cfg *cfg = dev->config;
#ifdef CONFIG_PM_DEVICE
	struct qtmr_qcc730_data *data = dev->data;
#endif
	ret = qtmr_qcc730_frame_init(dev);
	if (ret < 0) {
		LOG_ERR("Qtimer init failed err:%d", ret);
		return ret;
	}

	/* Enable interrupt */
	irq_enable(cfg->irqn);

#ifdef CONFIG_PM_DEVICE
	data->qtmr_initialized = true;
#endif

	return 0;
}

#ifdef CONFIG_PM_DEVICE

static int qtmr_qcc730_deinit(const struct device *dev)
{
	int ret = 0;
	const struct qtmr_qcc730_cfg *cfg = dev->config;
	struct qtmr_qcc730_data *data = dev->data;

	ret = qtmr_qcc730_frame_deinit(dev);
	if (ret < 0) {
		LOG_ERR("Qtimer deinit failed err:%d", ret);
		return ret;
	}

	/* Disable interrupt */
	irq_disable(cfg->irqn);

	data->qtmr_initialized = false;

	return 0;
}

static int qtmr_qcc730_pm_action(const struct device *dev, enum pm_device_action action)
{
	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		qtmr_qcc730_deinit(dev);
		break;
	case PM_DEVICE_ACTION_RESUME:
		qtmr_qcc730_init(dev);
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

#endif /* CONFIG_PM_DEVICE */

static DEVICE_API(counter, qtmr_qcc730_api) = {
	.start = qtmr_qcc730_start,
	.stop = qtmr_qcc730_stop,
	.get_value = qtmr_qcc730_get_value,
	.get_value_64 = qtmr_qcc730_get_value_64,
	.set_alarm = qtmr_qcc730_set_alarm,
	.cancel_alarm = qtmr_qcc730_cancel_alarm,
	.get_pending_int = qtmr_qcc730_get_pending_int,
	.get_freq = qtmr_qcc730_get_freq,
};

#define QTMR_QCC730_INIT(inst)                                                                     \
	static int qtmr_qcc730_init_##inst(const struct device *dev)                               \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(inst), DT_INST_IRQ(inst, priority),                       \
			    qtmr_qcc730_isr, DEVICE_DT_INST_GET(inst), 0);                         \
		return qtmr_qcc730_init(dev);                                                      \
	}                                                                                          \
	static const struct qtmr_qcc730_cfg qtmr_qcc730_cfg##inst = {                              \
		.irqn = DT_INST_IRQN(inst),                                                        \
		.info =                                                                            \
			{                                                                          \
				.max_top_value = UINT32_MAX,                                       \
				.freq = DT_PROP(DT_PARENT(DT_DRV_INST(inst)), clock_frequency),    \
				.flags = COUNTER_CONFIG_INFO_COUNT_UP,                             \
				.channels = 1,                                                     \
			},                                                                         \
		.frame_id = inst,                                                                  \
		.access_control_regs =                                                             \
			(QTMR_AC_BASE_qtmr_ac_Type *)DT_REG_ADDR(DT_PARENT(DT_DRV_INST(inst))),    \
		.qtmr_regs = (QTMR_V1_T0_BASE_qtmr_v1_t0_Type *)DT_REG_ADDR(DT_DRV_INST(inst)),    \
		.pmu_regs = (PMU_BASE_pmu_Type *)DT_REG_ADDR(DT_NODELABEL(pmu)),                   \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(inst)),                             \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(inst, id),             \
	};                                                                                         \
                                                                                                   \
	static struct qtmr_qcc730_data qtmr_qcc730_data##inst;                                     \
	PM_DEVICE_DT_INST_DEFINE(inst, qtmr_qcc730_pm_action);                                     \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, qtmr_qcc730_init_##inst, PM_DEVICE_DT_INST_GET(inst),          \
			      &qtmr_qcc730_data##inst,                                             \
			      &qtmr_qcc730_cfg##inst,                                              \
			      POST_KERNEL,                                                         \
			      CONFIG_COUNTER_QCC730_INIT_PRIORITY, &qtmr_qcc730_api);

DT_INST_FOREACH_STATUS_OKAY(QTMR_QCC730_INIT)
