# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: Apache-2.0

zephyr_library_amend()
zephyr_library_sources_ifdef(CONFIG_SPI_QCC730   spi_qcc730_spis.c)

# Inlcude spi_context.h
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/spi)
