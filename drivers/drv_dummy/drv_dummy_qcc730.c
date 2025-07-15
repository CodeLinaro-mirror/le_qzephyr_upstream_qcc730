/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qualcomm_drv_dummy_qcc730

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <soc.h>
#include <zephyr/drivers/drv_dummy/drv_dummy.h>

static int drv_dummy_qcc730_init(void)
{
	return 0;
}

SYS_INIT(drv_dummy_qcc730_init, PRE_KERNEL_1, 0);

