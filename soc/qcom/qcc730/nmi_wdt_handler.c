/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/arch/cpu.h>
#include <nt_sys_monitoring.h>
#include <fermion_hw_reg.h>

LOG_MODULE_REGISTER(nmi_wdt, LOG_LEVEL_ERR);

#if !defined(CONFIG_RUNTIME_NMI) || !defined(CONFIG_WATCHDOG)
#error "This file requires CONFIG_RUNTIME_NMI and CONFIG_WATCHDOG to be enabled"
#endif

/**
 * @brief NMI handler for Watchdog bark events
 *
 * AON Watchdog bark event triggers an NMI
 * interrupt rather than directly resetting the system. This handler
 * processes the bark event by clearing the bark signal and initiating
 * a software-controlled system reset.
 */
static void qcc730_wdt_nmi_handler(void)
{
	uint32_t control_reg;
	uint32_t wdog_status;

	/* Read watchdog status to confirm this is a watchdog bark */
	wdog_status = *(volatile uint32_t *)QWLAN_PMU_WDOG_STS_REG;

	/*
	 * AON Watchdog bark handling procedure:
	 * The bark event generates an NMI interrupt. To properly handle this,
	 * we must clear the bark signal before initiating a software reset.
	 */

	/* Clear Watchdog bark signal */
	control_reg = *(volatile uint32_t *)QWLAN_PMU_AON_WDOG_CTL_REG;
	control_reg |= QWLAN_PMU_AON_WDOG_CTL_WDOG_RESET_MASK;
	*(volatile uint32_t *)QWLAN_PMU_AON_WDOG_CTL_REG = control_reg;

	control_reg &= ~QWLAN_PMU_AON_WDOG_CTL_WDOG_RESET_MASK;
	*(volatile uint32_t *)QWLAN_PMU_AON_WDOG_CTL_REG = control_reg;

	/* Wait for count to reach zero */
	while (0 != *(volatile uint32_t *)QWLAN_PMU_AON_WDOG_COUNT_REG) {
		/* Busy wait */
	}

	/* Trigger platform-specific software reset */
	nt_system_sw_reset();

	/* Should not reach here */
	CODE_UNREACHABLE;
}

/**
 * @brief Initialize custom NMI handler for Watchdog
 */
static int qcc730_nmi_wdt_init(void)
{
	/* Register custom NMI handler */
	z_arm_nmi_set_handler(qcc730_wdt_nmi_handler);

	LOG_INF("QCC730 Watchdog NMI handler registered");

	return 0;
}

/* Initialize NMI handler early in boot process */
SYS_INIT(qcc730_nmi_wdt_init, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
