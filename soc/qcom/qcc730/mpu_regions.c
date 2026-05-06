/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/arch/arm/mpu/arm_mpu_mem_cfg.h>

/* RRAM region: 0x200000 - 0x380000 */
#define QCC730_RRAM_START   0x200000
#define QCC730_RRAM_SIZE    REGION_2M

/* RAM region: 0x00000 - 0xa0000 */
#define QCC730_RAM_START    0x000000
#define QCC730_RAM_SIZE     REGION_1M

/* RRAM: read/write/execute, write-through cache
 * 2MB region, subregion 6+7 (0x380000-0x400000) disabled via SRD
 * to match exact range 0x200000-0x380000
 */
#define REGION_RRAM_RWX_ATTR(size) \
	{(NORMAL_OUTER_INNER_WRITE_THROUGH_NON_SHAREABLE \
	  | size | P_RW_U_RW_Msk \
	  | SUB_REGION_6_DISABLED | SUB_REGION_7_DISABLED)}

/* RAM: read/write/execute, write-back cache
 * 1MB region, subregion 5+6+7 (0x0A0000-0x100000) disabled via SRD
 * to match exact range 0x000000-0x0A0000
 */
#define REGION_RAM_RWX_ATTR(size) \
	{(NORMAL_OUTER_INNER_WRITE_BACK_WRITE_READ_ALLOCATE_NON_SHAREABLE \
	  | size | P_RW_U_RW_Msk \
	  | SUB_REGION_5_DISABLED | SUB_REGION_6_DISABLED | SUB_REGION_7_DISABLED)}

/*
 * RRAM (QCC730_RRAM_START, 2MB, subregion 6+7 disabled = exact 1.5MB coverage):
 *   subregion size = 2MB/8 = 256KB
 *   SR0-SR5: 0x200000-0x380000 enabled
 *   SR6-SR7: 0x380000-0x400000 disabled
 *
 * RAM (QCC730_RAM_START, 1MB, subregion 5+6+7 disabled = 640KB coverage):
 *   subregion size = 1MB/8 = 128KB
 *   SR0-SR4: 0x000000-0x0A0000 enabled
 *   SR5-SR7: 0x0A0000-0x100000 disabled
 */
static const struct arm_mpu_region mpu_regions[] = {
	MPU_REGION_ENTRY("RRAM_0",
		QCC730_RRAM_START,
		REGION_RRAM_RWX_ATTR(QCC730_RRAM_SIZE)),

	MPU_REGION_ENTRY("RAM_0",
		QCC730_RAM_START,
		REGION_RAM_RWX_ATTR(QCC730_RAM_SIZE)),
};

const struct arm_mpu_config mpu_config = {
	.num_regions = ARRAY_SIZE(mpu_regions),
	.mpu_regions = mpu_regions,
};
