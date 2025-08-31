# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: Apache-2.0

zephyr_library_amend()
zephyr_library_sources_ifdef(CONFIG_RTC_QCC730     rtc_qcc730.c)

# Needed for rtc_utils.h
zephyr_include_directories(${ZEPHYR_BASE}/drivers/rtc)
