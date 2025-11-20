/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API extensions for the Qualcomm QCC730 Qtimer driver.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_COUNTER_QCC730_QTMR_H_
#define ZEPHYR_INCLUDE_DRIVERS_COUNTER_QCC730_QTMR_H_

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set a single shot alarm on a channel with absolute time.
 *
 * This function allows for setting an alarm with a 64-bit absolute time value.
 *
 * @note API is not thread safe.
 *
 * @param dev		Pointer to the device structure for the driver instance.
 * @param chan_id	Channel ID.
 * @param alarm_cfg	Alarm configuration; ticks are not used in this case
 * @param ticks	    Absolute value of ticks.
 *
 * @retval 0 If successful.
 * @retval -EINVAL if alarm settings are invalid.
 * @retval -EBUSY  if alarm is already active.
 */
int qtmr_qcc730_set_alarm_absolute(const struct device *dev, uint8_t chan_id,
                                   const struct counter_alarm_cfg *alarm_cfg,
                                   uint64_t ticks);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_COUNTER_QCC730_QTMR_H_ */
