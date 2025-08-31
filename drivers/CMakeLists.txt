# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_COUNTER counter)
add_subdirectory_ifdef(CONFIG_GPIO gpio)
add_subdirectory_ifdef(CONFIG_I2C i2c)
add_subdirectory_ifdef(CONFIG_PINCTRL pinctrl)
add_subdirectory_ifdef(CONFIG_RTC rtc)
add_subdirectory_ifdef(CONFIG_SERIAL serial)
add_subdirectory_ifdef(CONFIG_WATCHDOG watchdog)
add_subdirectory_ifdef(CONFIG_WIFI wifi)
