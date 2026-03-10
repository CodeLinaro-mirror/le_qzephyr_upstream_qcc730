/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

 #include <zephyr/kernel.h>
#include <kernel_internal.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/platform/hooks.h>
#include <zephyr/arch/cache.h>
#include <zephyr/arch/common/init.h>
#include <zephyr/arch/common/xip.h>

#include "nt_socpm_sleep.h"
#include "nt_sys_monitoring.h"

#if defined(__GNUC__)
/*
 * GCC can detect if memcpy is passed a NULL argument, however one of
 * the cases of relocate_vector_table() it is valid to pass NULL, so we
 * suppress the warning for this case.  We need to do this before
 * string.h is included to get the declaration of memcpy.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnonnull"
#endif

#include <string.h>

#if defined(CONFIG_SW_VECTOR_RELAY) || defined(CONFIG_SW_VECTOR_RELAY_CLIENT)
Z_GENERIC_SECTION(.vt_pointer_section) __attribute__((used)) void *_vector_table_pointer;
#endif

#ifdef CONFIG_CPU_CORTEX_M_HAS_VTOR

#define VECTOR_ADDRESS ((uintptr_t)_vector_start)

/* In some Cortex-M3 implementations SCB_VTOR bit[29] is called the TBLBASE bit */
#ifdef SCB_VTOR_TBLBASE_Msk
#define VTOR_MASK (SCB_VTOR_TBLBASE_Msk | SCB_VTOR_TBLOFF_Msk)
#else
#define VTOR_MASK SCB_VTOR_TBLOFF_Msk
#endif

static inline void relocate_vector_table(void)
{
	SCB->VTOR = VECTOR_ADDRESS & VTOR_MASK;
	barrier_dsync_fence_full();
	barrier_isync_fence_full();
}

#else
#define VECTOR_ADDRESS 0

void __weak relocate_vector_table(void)
{
#if defined(CONFIG_XIP) && (CONFIG_FLASH_BASE_ADDRESS != 0) ||                                     \
	!defined(CONFIG_XIP) && (CONFIG_SRAM_BASE_ADDRESS != 0)
	size_t vector_size = (size_t)_vector_end - (size_t)_vector_start;
	(void)memcpy(VECTOR_ADDRESS, _vector_start, vector_size);
#elif defined(CONFIG_SW_VECTOR_RELAY) || defined(CONFIG_SW_VECTOR_RELAY_CLIENT)
	_vector_table_pointer = _vector_start;
#endif
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#endif /* CONFIG_CPU_CORTEX_M_HAS_VTOR */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <soc.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <qlib_early_printk.h>
#include <qlib_util.h>
#include <zephyr/irq.h>
#include <qpower.h>

#include <zephyr/linker/sections.h>
#include <kernel_internal.h>

#include <zephyr/linker/linker-defs.h>
#include <zephyr/arch/common/init.h>

/* Function declarations to avoid implicit declaration warnings */
extern uint64_t nt_socpm_get_slp_tmr_us(void);
extern void nt_system_sw_reset(void);
extern void arch_bss_zero(void);
extern void arch_data_copy(void);

typedef struct {
    uint64_t bootup_slp_us;
} bootup_info_t;

extern SCB_Type *g_SCB;
extern NVIC_Type *g_NVIC;
extern SysTick_Type *g_SysTick;

static __noinit bootup_info_t bootup_info;
void rram_boot_early_init(void *sbl_args);

LOG_MODULE_REGISTER(soc, CONFIG_SOC_LOG_LEVEL);

void soc_early_reset_hook(void)
{
    void *sbl_args = NULL;
    
    __asm__ volatile (
        "mov %0, r0\n"
        : "=r" (sbl_args)
        :
        : "memory"
    );
    
    rram_boot_early_init(sbl_args);
    memset(&bootup_info, 0, sizeof(bootup_info_t));
    bootup_info.bootup_slp_us = nt_socpm_get_slp_tmr_us();
}

#if defined(CONFIG_CPU_HAS_FPU)
static inline void z_arm_floating_point_init(void)
{
	/*
	 * Upon reset, the Co-Processor Access Control Register is, normally,
	 * 0x00000000. However, it might be left un-cleared by firmware running
	 * before Zephyr boot.
	 */
	SCB->CPACR &= (~(CPACR_CP10_Msk | CPACR_CP11_Msk));

#if defined(CONFIG_FPU)
	/*
	 * Enable CP10 and CP11 Co-Processors to enable access to floating
	 * point registers.
	 */
#if defined(CONFIG_USERSPACE)
	/* Full access */
	SCB->CPACR |= CPACR_CP10_FULL_ACCESS | CPACR_CP11_FULL_ACCESS;
#else
	/* Privileged access only */
	SCB->CPACR |= CPACR_CP10_PRIV_ACCESS | CPACR_CP11_PRIV_ACCESS;
#endif  /* CONFIG_USERSPACE */
	/*
	 * Upon reset, the FPU Context Control Register is 0xC0000000
	 * (both Automatic and Lazy state preservation is enabled).
	 */
#if defined(CONFIG_MULTITHREADING) && !defined(CONFIG_FPU_SHARING)
	/* Unshared FP registers (multithreading) mode. We disable the
	 * automatic stacking of FP registers (automatic setting of
	 * FPCA bit in the CONTROL register), upon exception entries,
	 * as the FP registers are to be used by a single context (and
	 * the use of FP registers in ISRs is not supported). This
	 * configuration improves interrupt latency and decreases the
	 * stack memory requirement for the (single) thread that makes
	 * use of the FP co-processor.
	 */
	FPU->FPCCR &= (~(FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk));
#else
	/*
	 * FP register sharing (multithreading) mode or single-threading mode.
	 *
	 * Enable both automatic and lazy state preservation of the FP context.
	 * The FPCA bit of the CONTROL register will be automatically set, if
	 * the thread uses the floating point registers. Because of lazy state
	 * preservation the volatile FP registers will not be stacked upon
	 * exception entry, however, the required area in the stack frame will
	 * be reserved for them. This configuration improves interrupt latency.
	 * The registers will eventually be stacked when the thread is swapped
	 * out during context-switch or if an ISR attempts to execute floating
	 * point instructions.
	 */
	FPU->FPCCR = FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk;
#endif /* CONFIG_FPU_SHARING */

	/* Make the side-effects of modifying the FPCCR be realized
	 * immediately.
	 */
	barrier_dsync_fence_full();
	barrier_isync_fence_full();

	/* Initialize the Floating Point Status and Control Register. */
#if defined(CONFIG_ARMV8_1_M_MAINLINE)
	/*
	 * For ARMv8.1-M with FPU, the FPSCR[18:16] LTPSIZE field must be set
	 * to 0b100 for "Tail predication not applied" as it's reset value
	 */
	__set_FPSCR(4 << FPU_FPDSCR_LTPSIZE_Pos);
#else
	__set_FPSCR(0);
#endif

	/*
	 * Note:
	 * The use of the FP register bank is enabled, however the FP context
	 * will be activated (FPCA bit on the CONTROL register) in the presence
	 * of floating point instructions.
	 */

#endif /* CONFIG_FPU */

	/*
	 * Upon reset, the CONTROL.FPCA bit is, normally, cleared. However,
	 * it might be left un-cleared by firmware running before Zephyr boot.
	 * We must clear this bit to prevent errors in exception unstacking.
	 *
	 * Note:
	 * In Sharing FP Registers mode CONTROL.FPCA is cleared before switching
	 * to main, so it may be skipped here (saving few boot cycles).
	 *
	 * If CONFIG_INIT_ARCH_HW_AT_BOOT is set, CONTROL is cleared at reset.
	 */
#if (!defined(CONFIG_FPU) || !defined(CONFIG_FPU_SHARING)) &&                                      \
	(!defined(CONFIG_INIT_ARCH_HW_AT_BOOT))

	__set_CONTROL(__get_CONTROL() & (~(CONTROL_FPCA_Msk)));
#endif
}

#endif /* CONFIG_CPU_HAS_FPU */

typedef struct boot_sbl_share_s{
    uint32_t magic_num;
    uint8_t ver;
    uint8_t img_type;
    uint8_t rsv1;
    uint8_t rsv2;
    uint32_t bdf_addr;
    /* others ... */
} boot_sbl_share;

#define BOOT_MODE_FULL_LOAD  0x1
#define BOOT_MODE_RAM_LOAD   0x2
#define SBL_SHARE_MAGIC      0x55aa55aa

#define APP_ARGS_MAGIC	(0x55aa55aa)
enum ota_image_format {
    OTA_IMG_FORMAT_ELF,
    OTA_IMG_FORMAT_BIN,
};

#define SBL_SHARE_VER 1

void rram_boot_early_init(void *sbl_args)
{
    boot_sbl_share sbl_share = *(boot_sbl_share *)sbl_args;

    /* this may come from wdog reset */
    if(!(sbl_share.ver >= SBL_SHARE_VER))
        nt_system_sw_reset();
            
    uint8_t app_arg_bin_mode = sbl_share.img_type;
    uint32_t app_arg_magic = sbl_share.magic_num;

    uint8_t do_memload = 1;
    if ((APP_ARGS_MAGIC==app_arg_magic) && (OTA_IMG_FORMAT_ELF==app_arg_bin_mode)) {
        do_memload = 0;
    }
    if (do_memload==1) {
        arch_bss_zero();
        arch_data_copy();
    }
    else
    {
	    size_t bss_size = (size_t)(__bss_end - __bss_start);
	    memset(__bss_start, 0, bss_size);
    }
}

// Here interrupt is not enabled yet
void soc_prep_hook(void)
{
    // For zephyr.bin, no loader to initialize bss&data, then bss & data are not initialzed yet
    // dead_loop();

    // actually, this is the realization of z_prep_c(), just process the bss & data init here in case of elf load.

    relocate_vector_table();
#if defined(CONFIG_CPU_HAS_FPU)
    z_arm_floating_point_init();
#endif
    /*
    z_bss_zero();
    z_data_copy();
    */
#if defined(CONFIG_ARM_CUSTOM_INTERRUPT_CONTROLLER)
    /* Invoke SoC-specific interrupt controller initialization */
    z_soc_irq_init();
#else
    z_arm_interrupt_init();
#endif /* CONFIG_ARM_CUSTOM_INTERRUPT_CONTROLLER */
#if CONFIG_ARCH_CACHE
    arch_cache_init();
#endif

#ifdef CONFIG_NULL_POINTER_EXCEPTION_DETECTION_DWT
    z_arm_debug_enable_null_pointer_detection();
#endif

    z_cstart();
    CODE_UNREACHABLE;
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
