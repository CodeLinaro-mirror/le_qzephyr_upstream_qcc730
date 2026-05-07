/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "soc.h"
#include "qpower.h"

#ifdef CONFIG_ARM_MPU
#include <zephyr/arch/arm/mpu/arm_mpu.h>
static struct z_mpu_context_retained mpu_context;
#endif

LOG_MODULE_REGISTER(soc_power, CONFIG_SOC_LOG_LEVEL);

void pm_state_set(enum pm_state state, uint8_t substate_id)
{
    switch (state) {
    case PM_STATE_SOFT_OFF:
        LOG_INF("enter SOFT_OFF, will go deepsleep!");
        qapi_enter_softoff();
        break;
    case PM_STATE_SUSPEND_TO_RAM:
        LOG_INF("enter SUSPEND_TO_RAM, will go s2ram!");
#ifdef CONFIG_ARM_MPU
        /* mcu_sleep_enter() moves vector table to 0x0; disable MPU
         * first so the null-pointer guard region does not fault. */
        z_arm_save_mpu_context(&mpu_context);
        arm_core_mpu_disable();
#endif
        qapi_enter_suspend2ram();
        break;
    default:
	break;
    }
}

void pm_state_exit_post_ops(enum pm_state state, uint8_t substate_id)
{
    if (state == PM_STATE_SUSPEND_TO_RAM) {
        qapi_suspend2ram_exit_post_ops();
#ifdef CONFIG_ARM_MPU
        /* Hardware reset during sleep clears MPU registers; restore
         * all region configs and re-enable MPU. */
        z_arm_restore_mpu_context(&mpu_context);
#endif
    }
}
