# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: Apache-2.0

zephyr_library_amend()
zephyr_library_sources_ifdef(CONFIG_COUNTER_QCC730 counter_qcc730_qtmr.c)
zephyr_library_sources_ifdef(CONFIG_COUNTER_QCC730_AON counter_qcc730_aon.c)
