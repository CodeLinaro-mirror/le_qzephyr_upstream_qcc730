# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: Apache-2.0;
zephyr_library()

zephyr_library_sources_ifdef(CONFIG_DRV_DUMMY_QCC730	drv_dummy_qcc730.c)