/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qcom_qcc730_rtc

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/counter/counter_qcc730_qtmr.h>
#include <zephyr/sys/timeutil.h>
#include <zephyr/logging/log.h>
#include <rtc_utils.h>
#include <soc.h>

LOG_MODULE_REGISTER(rtc_qcc730, CONFIG_RTC_LOG_LEVEL);

#define RTC_QCC730_TIME_MASK                                                                       \
	(RTC_ALARM_TIME_MASK_SECOND | RTC_ALARM_TIME_MASK_MINUTE | RTC_ALARM_TIME_MASK_HOUR |      \
	 RTC_ALARM_TIME_MASK_MONTHDAY | RTC_ALARM_TIME_MASK_MONTH | RTC_ALARM_TIME_MASK_YEAR |     \
	 RTC_ALARM_TIME_MASK_NSEC)

struct rtc_qcc730_config {
	const struct device *qtimer_frame;
#ifdef CONFIG_RTC_ALARM
	uint8_t alarms_count;
#endif /* CONFIG_RTC_ALARM */
};

struct rtc_qcc730_data {
	int64_t set_sec;
	int64_t set_ns;
	uint64_t set_qtimer_cnt;
	bool time_set;
	struct k_spinlock lock;
#ifdef CONFIG_RTC_ALARM
	struct rtc_time alarm_time;
	rtc_alarm_callback alarm_callback;
	void *alarm_user_data;
	bool alarm_is_pending;
	uint16_t alarm_mask;
#endif /* CONFIG_RTC_ALARM */
};

#ifdef CONFIG_RTC_ALARM
/**
 * @brief RTC alarm callback
 *
 * It is being triggered by qtimer alarm activation.
 *
 * @param[in] dev        Pointer to the qtimer device
 * @param[in] chan_id    Channel id (it should always be 0)
 * @param[in] ticks      Number of ticks for which alarm was set
 * @param[in] user_data  User data specified when scheduling an alarm
 */
static void rtc_qcc730_counter_alarm_callback(const struct device *dev, uint8_t chan_id,
					      uint32_t ticks, void *user_data)
{
	LOG_DBG("RTC received the Qtimer alarm callback.");

	// dev here is counter device
	struct device *rtc_dev = (struct device *) user_data;
	struct rtc_qcc730_data *data = (struct rtc_qcc730_data *)rtc_dev->data;

	k_spinlock_key_t key = k_spin_lock(&data->lock);

	if (!data->alarm_callback) {
		data->alarm_is_pending = true;
		k_spin_unlock(&data->lock, key);
		return;
	}

	data->alarm_callback(dev, chan_id, data->alarm_user_data);
	k_spin_unlock(&data->lock, key);
}


/**
 * @brief Shared functionality of setting alarm time
 *
 * This helper function should be used only in context with spinlock locked.
 * It sets the alarm with provided parameters.
 *
 * @param[in] dev        Pointer to the qtimer device
 * @param[in] chan_id    Channel id (it should always be 0)
 * @param[in] ticks      Number of ticks for which alarm was set
 * @param[in] user_data  User data specified when scheduling an alarm
 */
static int rtc_qcc730_alarm_set_time_shared(const struct device *dev, uint16_t id, uint16_t mask,
					    const struct rtc_time *timeptr)
{
	const struct rtc_qcc730_config *cfg = dev->config;
	struct rtc_qcc730_data *data = dev->data;
	struct counter_alarm_cfg alarm_cfg;
	int64_t alarm_sec, alarm_ns;
	int64_t diff_sec, diff_ns;
	uint64_t freq, ticks;
	uint64_t ticks_now = 0ULL;
	int ret;

	alarm_sec = timeutil_timegm64((const struct tm *)timeptr);
	alarm_ns = timeptr->tm_nsec;

	if (alarm_sec < data->set_sec || (alarm_sec == data->set_sec && alarm_ns < data->set_ns)) {
		LOG_ERR("Trying to set RTC alarm in the past.");
		return -EINVAL;
	}

	diff_sec = alarm_sec - data->set_sec;
	diff_ns = alarm_ns - data->set_ns;

	if (diff_ns < 0) {
		diff_ns += (int64_t)NSEC_PER_SEC;
		diff_sec -= 1;
	}

	// Convert time to ticks
	freq = counter_get_frequency(cfg->qtimer_frame);
	ticks = diff_sec * (uint64_t)freq;
	ticks += ((uint64_t)diff_ns * (uint64_t)freq) / NSEC_PER_SEC;

	ret = counter_get_value_64(cfg->qtimer_frame, &ticks_now);
	if (ret < 0) {
		return -EIO;
	}

	// Add current ticks to the alarm ticks value
	ticks += ticks_now;

	// Alarm configuration
	alarm_cfg.callback = rtc_qcc730_counter_alarm_callback;
	alarm_cfg.flags = COUNTER_ALARM_CFG_ABSOLUTE;
	alarm_cfg.user_data = (void *)dev;

	ret = qtmr_qcc730_set_alarm_absolute(cfg->qtimer_frame, 0, &alarm_cfg, ticks);
	if (ret < 0) {
		return -EIO;
	}

	// Save alarm time and mask in case of new alarm
	if (&data->alarm_time != timeptr) {
		// memcpy is not allowed
		data->alarm_time.tm_sec = timeptr->tm_sec;
		data->alarm_time.tm_min = timeptr->tm_min;
		data->alarm_time.tm_hour = timeptr->tm_hour;
		data->alarm_time.tm_mday = timeptr->tm_mday;
		data->alarm_time.tm_mon = timeptr->tm_mon;
		data->alarm_time.tm_year = timeptr->tm_year;
		data->alarm_time.tm_wday = timeptr->tm_wday;
		data->alarm_time.tm_yday = timeptr->tm_yday;
		data->alarm_time.tm_isdst = timeptr->tm_isdst;
		data->alarm_time.tm_nsec = timeptr->tm_nsec;

		data->alarm_mask = mask;
	}

	return 0;
}
#endif

static int rtc_qcc730_set_time(const struct device *dev, const struct rtc_time *timeptr)
{
	const struct rtc_qcc730_config *cfg = dev->config;
	struct rtc_qcc730_data *data = dev->data;

	if (rtc_utils_validate_rtc_time(timeptr, RTC_QCC730_TIME_MASK) == false) {
		LOG_ERR("Wrong time provided");
		return -EINVAL;
	}

	int ret = counter_get_value_64(cfg->qtimer_frame, &data->set_qtimer_cnt);
	if (ret < 0) {
		LOG_ERR("Failed to set time for RTC");
		return ret;
	}

	k_spinlock_key_t key = k_spin_lock(&data->lock);

	data->set_sec = timeutil_timegm64((const struct tm *)timeptr);
	data->set_ns = timeptr->tm_nsec;

#ifdef CONFIG_RTC_ALARM
	// If there is alarm set - we need to update it
	if (data->alarm_mask != 0) {
		counter_cancel_channel_alarm(cfg->qtimer_frame, 0);

		ret = rtc_qcc730_alarm_set_time_shared(dev,
						       0,
						       data->alarm_mask,
						       (const struct rtc_time *)&data->alarm_time);
		if (ret < 0) {
			k_spin_unlock(&data->lock, key);
			LOG_ERR("Failed to update scheduled alarm");
			return ret;
		}
	}
#endif

	data->time_set = true;

	k_spin_unlock(&data->lock, key);

	return 0;
}

static int rtc_qcc730_get_time(const struct device *dev, struct rtc_time *timeptr)
{
	const struct rtc_qcc730_config *cfg = dev->config;
	struct rtc_qcc730_data *data = dev->data;
	uint64_t cnt_now = 0ULL;

	if (!data->time_set) {
		LOG_ERR("RTC time has not been set yet.");
		return -ENODATA;
	}

	k_spinlock_key_t key = k_spin_lock(&data->lock);

	int ret = counter_get_value_64(cfg->qtimer_frame, &cnt_now);
	if (ret < 0) {
		LOG_ERR("Failed to get time from RTC.");
		k_spin_unlock(&data->lock, key);
		return ret;
	}

	const uint32_t freq = counter_get_frequency(cfg->qtimer_frame);
	if (freq == 0U) {
		k_spin_unlock(&data->lock, key);
		return -EIO;
	}

	// Elapsed ticks since the base instant
	const uint64_t ticks_elapsed = cnt_now - data->set_qtimer_cnt;

	// Split to seconds + fractional nanoseconds
	const uint64_t elapsed_sec = ticks_elapsed / freq;
	const uint64_t rem_ticks = ticks_elapsed - (elapsed_sec * freq);

	// Convert fractional ticks to nanoseconds
	const uint64_t frac_ns = (rem_ticks * NSEC_PER_SEC) / freq;

	// Calculate elapsed nanoseconds
	uint64_t now_ns_total = (uint64_t)data->set_ns + frac_ns;
	uint64_t carry_sec = now_ns_total / NSEC_PER_SEC;
	uint32_t now_nsec = (uint32_t)(now_ns_total - carry_sec * NSEC_PER_SEC);

	int64_t now_sec = data->set_sec + (int64_t)elapsed_sec + (int64_t)carry_sec;

	struct tm tm_now;
	gmtime_r(&now_sec, &tm_now);

	timeptr->tm_sec = tm_now.tm_sec;
	timeptr->tm_min = tm_now.tm_min;
	timeptr->tm_hour = tm_now.tm_hour;
	timeptr->tm_mday = tm_now.tm_mday;
	timeptr->tm_mon = tm_now.tm_mon;
	timeptr->tm_year = tm_now.tm_year;
	timeptr->tm_wday = tm_now.tm_wday;
	timeptr->tm_yday = tm_now.tm_yday;
	timeptr->tm_nsec = now_nsec;
	// Daylight Saving Time status unknown
	timeptr->tm_isdst = -1;

	k_spin_unlock(&data->lock, key);

	return 0;
}

#ifdef CONFIG_RTC_ALARM
static int rtc_qcc730_alarm_get_supported_fields(const struct device *dev, uint16_t id,
						 uint16_t *mask)
{
	ARG_UNUSED(id);
	const struct rtc_qcc730_config *cfg = dev->config;

	if (id > (cfg->alarms_count - 1)) {
		LOG_ERR("Invalid alarm id provided: %u", id);
		return -EINVAL;
	}

	*mask = (RTC_ALARM_TIME_MASK_SECOND | RTC_ALARM_TIME_MASK_MINUTE |
		 RTC_ALARM_TIME_MASK_HOUR | RTC_ALARM_TIME_MASK_MONTHDAY |
		 RTC_ALARM_TIME_MASK_MONTH | RTC_ALARM_TIME_MASK_YEAR);

	return 0;
}

static int rtc_qcc730_alarm_set_time(const struct device *dev, uint16_t id, uint16_t mask,
				     const struct rtc_time *timeptr)
{
	const struct rtc_qcc730_config *cfg = dev->config;
	struct rtc_qcc730_data *data = dev->data;
	int ret = 0;

	k_spinlock_key_t key = k_spin_lock(&data->lock);

	if (mask == 0 || !timeptr) {
		counter_cancel_channel_alarm(cfg->qtimer_frame, 0);
		memset(&data->alarm_time, 0, sizeof(struct rtc_time));
		data->alarm_mask = 0;
		k_spin_unlock(&data->lock, key);
		return 0;
	}

	if (id > 0) {
		LOG_ERR("Only 1 alarm is supported by RTC.");
		k_spin_unlock(&data->lock, key);
		return -EINVAL;
	}

	if (rtc_utils_validate_rtc_time(timeptr, mask) == false) {
		LOG_ERR("Wrong time provided for alarm");
		k_spin_unlock(&data->lock, key);
		return -EINVAL;
	}

	ret = rtc_qcc730_alarm_set_time_shared(dev, id, mask, timeptr);
	if (ret < 0) {
		LOG_ERR("Failed to set new alarm");
		k_spin_unlock(&data->lock, key);
		return ret;
	}

	k_spin_unlock(&data->lock, key);

	return 0;
}

static int rtc_qcc730_alarm_get_time(const struct device *dev, uint16_t id, uint16_t *mask,
				     struct rtc_time *timeptr)
{
	struct rtc_qcc730_data *data = dev->data;
	uint16_t curr_alarm_mask = data->alarm_mask;
	uint16_t return_mask = 0;

	if (id != 0 || !timeptr || !mask) {
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&data->lock);

	if (curr_alarm_mask & RTC_ALARM_TIME_MASK_SECOND) {
		timeptr->tm_sec = data->alarm_time.tm_sec;
		return_mask |= RTC_ALARM_TIME_MASK_SECOND;
	}

	if (curr_alarm_mask & RTC_ALARM_TIME_MASK_MINUTE) {
		timeptr->tm_min = data->alarm_time.tm_min;
		return_mask |= RTC_ALARM_TIME_MASK_MINUTE;
	}

	if (curr_alarm_mask & RTC_ALARM_TIME_MASK_HOUR) {
		timeptr->tm_hour = data->alarm_time.tm_hour;
		return_mask |= RTC_ALARM_TIME_MASK_HOUR;
	}

	if (curr_alarm_mask & RTC_ALARM_TIME_MASK_MONTHDAY) {
		timeptr->tm_mday = data->alarm_time.tm_mday;
		return_mask |= RTC_ALARM_TIME_MASK_MONTHDAY;
	}

	if (curr_alarm_mask & RTC_ALARM_TIME_MASK_MONTH) {
		timeptr->tm_mon = data->alarm_time.tm_mon;
		return_mask |= RTC_ALARM_TIME_MASK_MONTH;
	}

	if (curr_alarm_mask & RTC_ALARM_TIME_MASK_YEAR) {
		timeptr->tm_year = data->alarm_time.tm_year;
		return_mask |= RTC_ALARM_TIME_MASK_YEAR;
	}

	*mask = return_mask;

	k_spin_unlock(&data->lock, key);

	return 0;
}

static int rtc_qcc730_alarm_is_pending(const struct device *dev, uint16_t id)
{
	struct rtc_qcc730_data *data = dev->data;
	int ret = 0;

	k_spinlock_key_t key = k_spin_lock(&data->lock);

	ret = data->alarm_is_pending ? 1 : 0;
	data->alarm_is_pending = false;

	k_spin_unlock(&data->lock, key);

	return ret;
}

static int rtc_qcc730_alarm_set_callback(const struct device *dev, uint16_t id,
					 rtc_alarm_callback callback, void *user_data)
{
	struct rtc_qcc730_data *data = dev->data;

	LOG_DBG("DBG: set callback function");

	k_spinlock_key_t key = k_spin_lock(&data->lock);

	// Callback can be set to NULL if needed
	data->alarm_callback = callback;
	data->alarm_user_data = user_data;

	k_spin_unlock(&data->lock, key);

	return 0;
}

#endif /* CONFIG_RTC_ALARM */

static int rtc_qcc730_init(const struct device *dev)
{
	const struct rtc_qcc730_config *cfg = dev->config;
#ifdef CONFIG_RTC_ALARM
	struct rtc_qcc730_data *data = dev->data;
#endif

	if (!device_is_ready(cfg->qtimer_frame)) {
		LOG_ERR("Qtimer frame for RTC not ready.");
		return -ENODEV;
	}

#ifdef CONFIG_RTC_ALARM
	if (cfg->alarms_count != 1) {
		LOG_ERR("Only one alarm is supported by the qtimer. Please change alarm count to 1.");
		return -EINVAL;
	}

	data->alarm_is_pending = false;
	data->alarm_callback = NULL;
	data->alarm_mask = 0;
#endif

	int ret = counter_start(cfg->qtimer_frame);

	if (ret < 0) {
		LOG_ERR("Failed to start the Qtimer frame for RTC.");
		return ret;
	}

	return 0;
}

static DEVICE_API(rtc, rtc_qcc730_api) = {
	.set_time = rtc_qcc730_set_time,
	.get_time = rtc_qcc730_get_time,
#ifdef CONFIG_RTC_ALARM
	.alarm_get_supported_fields = rtc_qcc730_alarm_get_supported_fields,
	.alarm_set_time = rtc_qcc730_alarm_set_time,
	.alarm_get_time = rtc_qcc730_alarm_get_time,
	.alarm_is_pending = rtc_qcc730_alarm_is_pending,
	.alarm_set_callback = rtc_qcc730_alarm_set_callback,
#endif /* CONFIG_RTC_ALARM */
};

#ifdef CONFIG_RTC_ALARM
#define QCC730_RTC_INIT(inst)                                                                      \
	static const struct rtc_qcc730_config rtc_qcc730_config_##inst = {                         \
		.qtimer_frame = DEVICE_DT_GET(DT_INST_PHANDLE(inst, qtimer_frame)),                \
		.alarms_count = DT_INST_PROP(inst, alarms_count)                                   \
	};                                                                                         \
	static struct rtc_qcc730_data rtc_qcc730_data_##inst;                                      \
	DEVICE_DT_INST_DEFINE(inst, rtc_qcc730_init, NULL, &rtc_qcc730_data_##inst,                \
			      &rtc_qcc730_config_##inst, POST_KERNEL,                              \
			      CONFIG_RTC_QCC730_INIT_PRIORITY, &rtc_qcc730_api);
#else
#define QCC730_RTC_INIT(inst)                                                                      \
	static const struct rtc_qcc730_config rtc_qcc730_config_##inst = {                         \
		.qtimer_frame = DEVICE_DT_GET(DT_INST_PHANDLE(inst, qtimer_frame))                \
	};                                                                                         \
	static struct rtc_qcc730_data rtc_qcc730_data_##inst;                                      \
	DEVICE_DT_INST_DEFINE(inst, rtc_qcc730_init, NULL, &rtc_qcc730_data_##inst,                \
			      &rtc_qcc730_config_##inst, POST_KERNEL,                              \
			      CONFIG_RTC_QCC730_INIT_PRIORITY, &rtc_qcc730_api);
#endif
DT_INST_FOREACH_STATUS_OKAY(QCC730_RTC_INIT)
