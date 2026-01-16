/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qcom_qcc730_aon_timer

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/logging/log.h>
#include <hal_aon.h>
#include <aon_tmr_mgr.h>
#include <qurt_timer.h>

#define LOG_MODULE_NAME 		counter_qcc730
LOG_MODULE_REGISTER(LOG_MODULE_NAME, CONFIG_COUNTER_LOG_LEVEL);

#define AON_MAX_TOP_VALUE		((1ull << 48) - 1)

typedef void(*irq_init_t)();

extern void nt_socpm_slp_tmr_set(uint64_t sleep_time);

struct qcc730_counter_config {
	struct counter_config_info counter_info;
	irq_init_t irq_init;
};

struct qcc730_counter_data {
	uint32_t chan_id;
	struct counter_alarm_cfg alarm_cfg;
};

static int qcc730_counter_get_value(const struct device *dev, uint32_t *ticks)
{

	*ticks = counter_us_to_ticks(dev,hres_timer_curr_time_us());

	return 0;
}

static int qcc730_counter_get_value_64(const struct device *dev, uint64_t *ticks)
{
	/* TODO: change to uint64_t format*/
	*ticks = counter_us_to_ticks(dev,hres_timer_curr_time_us());
	return 0;
}

static int qcc730_counter_stop(const struct device *dev)
{
	qaon_stop_count();
	return 0;
}

static int qcc730_counter_start(const struct device *dev)
{
	qaon_start_count();
	return 0;
}

static int set_top_value(const struct device *dev, const struct counter_top_cfg *cfg)
{
	return -ENOTSUP;
}

static uint32_t get_top_value(const struct device *dev)
{
	const struct qcc730_counter_config *config = dev->config;
	return config->counter_info.max_top_value;
}

static uint32_t get_pending_int(const struct device *dev)
{
	return -ENOTSUP;
}

static inline int set_alarm(const struct device *dev, uint8_t chan_id,
                            const struct counter_alarm_cfg *alarm_cfg)
{
	struct qcc730_counter_data *data = dev->data;
	data->chan_id = chan_id;
	data->alarm_cfg = *alarm_cfg;
	
	uint32_t tick_us = counter_ticks_to_us(dev,alarm_cfg->ticks);

	extern aon_sleep_info_t last_sleep_info;	
	aon_timer_set(AON_CLIENT_OS,(uint64_t)tick_us);
	aon_get_min_expiry(&last_sleep_info);
	
	nt_socpm_slp_tmr_set(((uint64_t)last_sleep_info.sleep_us));

	return 0;
}

static inline int cancel_alarm(const struct device *dev, uint8_t chan_id)
{
	qaon_set_alarm(AON_MAX_TOP_VALUE);
	return 0;
}

uint32_t qcc_counter_get_freq(const struct device *dev)
{
	const struct qcc730_counter_config *config = dev->config;
	return config->counter_info.freq;
}

static void qcc730_counter_isr(void *arg)
{
	struct device *dev = arg;
	struct qcc730_counter_data *data = dev->data;

	if (data->alarm_cfg.callback) {
		uint32_t now = 0;
		uint32_t chan_id = data->chan_id;

		qcc730_counter_get_value(dev, &now);
		data->alarm_cfg.callback(dev, chan_id, now, data->alarm_cfg.user_data);
	}
	qaon_clear_interrupt();
}

static int qcc730_counter_init(const struct device *dev)
{
	const struct qcc730_counter_config *config = dev->config;

	config->irq_init();

	aon_manager_init(hres_timer_curr_time_us);
	return 0;
}

const static struct counter_driver_api qcc_counter_driver = {
	.get_value = qcc730_counter_get_value,
	.get_value_64 = qcc730_counter_get_value_64,
	.start = qcc730_counter_start,
	.stop = qcc730_counter_stop,
	.set_top_value = set_top_value,
	.get_top_value = get_top_value,
	.get_pending_int = get_pending_int,
	.set_alarm = set_alarm,
	.cancel_alarm = cancel_alarm,
	.get_freq = qcc_counter_get_freq,
};

#define QCC730_COUNTER_DEVICE_INIT(instance)                                                                           \
    static void irq_config_##instance()                                                                                \
    {                                                                                                                  \
        IRQ_CONNECT(DT_INST_IRQN(instance), DT_INST_IRQ(instance, priority), qcc730_counter_isr,                       \
                    DEVICE_DT_INST_GET(instance), 0);                                                                  \
        irq_enable(DT_INST_IRQN(instance));                                                                            \
    }                                                                                                                  \
    static struct qcc730_counter_config qcc730_counter_config_##instance = {                                           \
        .counter_info = {.max_top_value = UINT32_MAX, .freq = 32 * 1024, .channels = 1},                               \
        .irq_init = irq_config_##instance,                                                                             \
    };                                                                                                                 \
    static struct qcc730_counter_data qcc730_counter_data_##instance = {};                                             \
    DEVICE_DT_INST_DEFINE(instance, qcc730_counter_init, NULL, &qcc730_counter_data_##instance,                        \
                          &qcc730_counter_config_##instance, POST_KERNEL, CONFIG_COUNTER_INIT_PRIORITY,                \
                          &qcc_counter_driver);

DT_INST_FOREACH_STATUS_OKAY(QCC730_COUNTER_DEVICE_INIT)
