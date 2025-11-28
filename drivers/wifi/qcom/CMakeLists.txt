# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: Apache-2.0;
zephyr_library_sources_ifdef(CONFIG_WIFI_QCOM qcom_wifi_drv.c)
zephyr_library_sources_ifdef(CONFIG_WIFI_QCOM qcom_wifi_mgmt.c)
zephyr_include_directories(inc)