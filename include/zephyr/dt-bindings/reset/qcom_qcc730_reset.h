/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_RESET_QCOM_QCC730_RESET_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_RESET_QCOM_QCC730_RESET_H_

#define QCC730_PMU_SOFT_RESET 0
#define QCC730_MCU_SOFT_RESET 1

/**
 * Pack reset register identifier and reset bit in one 32-bit value.
 *
 * 5 LSBs are used to store the rest bit number in the register (0-31).
 * The next bit encodes the register (0 - PMU_SOFT_RESET, 1 - MCU_SOFT_RESET).
 * The remaining bits are unused.
 *
 * @param reg QCC730 reset register name (MCU_SOFT_RESET or PMU_SOFT_RESET)
 * @param bit Reset bit
 */
#define QCC730_RESET(reg, bit) (((QCC730_##reg) << 5U) | (bit))

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_RESET_QCOM_QCC730_RESET_H_ */