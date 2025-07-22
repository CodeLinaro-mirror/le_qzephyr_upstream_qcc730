/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <soc.h>

#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <qlib_early_printk.h>
#include <qlib_util.h>
#include <qpower.h>

#include <zephyr/linker/sections.h>
#include <kernel_internal.h>

typedef struct {
    uint64_t bootup_slp_us;
} bootup_info_t;

static __noinit bootup_info_t bootup_info;

LOG_MODULE_REGISTER(soc, CONFIG_SOC_LOG_LEVEL);

void soc_reset_hook(void)
{
    z_early_memset(&bootup_info, 0, sizeof(bootup_info_t));
    bootup_info.bootup_slp_us = nt_socpm_get_slp_tmr_us();
}

// Here interrupt is not enabled yet
void soc_prep_hook(void)
{
    // For zephyr.bin, no loader to initialize bss&data, then bss & data are not initialzed yet
    // dead_loop();
}

// Here for zephyr.bin, bss & data are initialzed
static int qcc730_init_early(void)
{
    // Disable the system tick
    // g_SysTick->CTRL = 0;
    // Clear COUNTFLAG
    // g_SysTick->VAL = 0;

    early_printk_init();
    early_printk("%s - %s: %s Zephyr River try\r\n", __DATE__, __TIME__, __FUNCTION__);
    early_printk("g_SysTick->CTRL=0x%x g_NVIC->ISER[0]=0x%x g_NVIC->ISER[1]=0x%x g_SCB->VTOR=0x%x\r\n", g_SysTick->CTRL,
                 g_NVIC->ISER[0], g_NVIC->ISER[1], g_SCB->VTOR);
    early_printk("bootup_slp_us=%llu us\r\n", bootup_info.bootup_slp_us);
    // uart_echo(1);
    // uart_echo(0);
    return 0;
}

void soc_early_init_hook(void) { early_printk("%s\r\n", __FUNCTION__); }

#include <zephyr/logging/log.h>

/**
 * @brief Perform basic hardware initialization at boot.
 *
 * This needs to be run from the very beginning.
 * So the init priority has to be 0 (zero).
 *
 * @return 0
 */
static int qcc730_init_pre_kernel_1(void)
{
    early_printk("%s\r\n", __FUNCTION__);
    return 0;
}

static int qcc730_init_pre_kernel_2(void)
{
    LOG_ERR("%s", __FUNCTION__);
    qapi_pmu_init();
    return 0;
}

static int qcc730_init_post_kernel(void)
{
    LOG_WRN("%s", __FUNCTION__);
    return 0;
}

SYS_INIT(qcc730_init_early, EARLY, 0);
// Here interrupt is enabled already
// uart_console_init & __printk_hook_install are PRE_KERNEL_1
SYS_INIT(qcc730_init_pre_kernel_1, PRE_KERNEL_1, 0);
SYS_INIT(qcc730_init_pre_kernel_2, PRE_KERNEL_2, 0);
SYS_INIT(qcc730_init_post_kernel, POST_KERNEL, 0);
