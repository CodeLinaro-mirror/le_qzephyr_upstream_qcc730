/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "soc.h"
#include "qpower.h"

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
        qapi_enter_suspend2ram();
        break;
    }
}

void pm_state_exit_post_ops(enum pm_state state, uint8_t substate_id)
{
    if (state == PM_STATE_SUSPEND_TO_RAM) {
        qapi_suspend2ram_exit_post_ops();
    }
}
