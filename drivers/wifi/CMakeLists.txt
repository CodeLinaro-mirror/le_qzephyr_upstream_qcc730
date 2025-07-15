# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: Apache-2.0;
zephyr_library()
zephyr_library_property(ALLOW_EMPTY TRUE)

add_subdirectory_ifdef(CONFIG_WIFI_QUALCOMM       qualcomm)
