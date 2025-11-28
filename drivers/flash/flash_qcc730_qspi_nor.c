/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qcom_qspi_qcc730_nor

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/pm/device.h>
#include "ferm_qspi.h"
#include "ferm_flash.h"
#include <string.h>

#define SIZE_64K_BYTES                    KB(64U)
#define QCC730_FLASH_QUAD_MODE145_ENABLE_BIT 1
#define QCC730_FLASH_QUAD_MODE2_ENABLE_BIT   6
#define QCC730_FLASH_QUAD_MODE3_ENABLE_BIT   7

#define POWERUP_OPCODE   0xAB
#define DEEPSLEEP_OPCODE 0xB9

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(flash_qcc730, CONFIG_FLASH_LOG_LEVEL);

struct flash_qcc730_data {
	flash_context_t flash_ctx_data;
#if defined(CONFIG_MULTITHREADING)
	struct k_sem sem;
#endif
#ifdef CONFIG_PM_DEVICE
	bool qspi_initialized;
#endif
};

struct flash_qcc730_config {
	uint32_t device_id;
	uint32_t size;
	struct flash_pages_layout page_layout;
};

static const struct flash_parameters flash_qcc730_parameters = {
	.write_block_size = CONFIG_FLASH_QCC730_QSPI_WRITE_BLOCK_SIZE,
	.erase_value = 0xff, // same for all NOR flash devices
};

static int drv_flash_check_error(const struct device *dev, uint8_t operation_type);
static int drv_flash_read_reg_internal(uint8_t reg_opcode, uint8_t len, uint8_t *reg_value);
static int drv_flash_write_reg_internal(const struct device *dev, uint8_t reg_opcode, uint8_t len,
					uint8_t *reg_value);
static int drv_flash_wait_operation_done(const struct device *dev, uint32_t timeout,
					 uint32_t status_polling_usec, uint8_t operation,
					 uint8_t bmask, uint8_t status_value);

#define FLASH_SEM_TIMEOUT (k_is_in_isr() ? K_NO_WAIT : K_FOREVER)

#if CONFIG_FLASH_QCC730_QSPI_QUAD_MODE
/**
   @brief Quad enable mode 1, 4, 5.

   Mode 1: QE is bit 1 of status register 2. It is set via Write status  with
   two data bytes where bit 1 of the second byte is one. It is cleared via
   Write status with two data bytes where bit 1 of the second byte is zero.
   Writing only one byte to the status register has the side-effect of clearing
   status register 2, including the QE bit. The 100b code is used if writing
   one byte to the status register does not modify status register 2.
   Mode 4: QE is bit 1 of status register 2. It is set via Write status  with
   two data byte where bit 1 of the second byte is one. It is cleared via
   Write status  with two data bytes where bit 1 of the second byte is zero.
   In constrast to the ENABLE_QUAD_MODE_1, writing one byte to the status
   register does not modify status register 2.
   Mode 5: QE is bit 1 of the status register 2. status  register 1 is read
   using instruction 05h. status  register 2 is read using instruction 35h.
   QE is set via Write status  instruction 01h with two data bytes where bit 1
   of the second byte is one. It is cleared via Write status  with two data
   bytes where bit 1 of the second byte is zero.

   see for example GT25Q32A-U datasheet:
   https://www.giantec-semi.com/uploads/241023/gg/1.65~3.6V/GT25Q32A%20U%20DS_V2.0.pdf

   @return 0 on success or negative error code on failure.
*/
static int drv_flash_enable_quad_mode145(const struct device *dev)
{
	int status = 0;
	uint8_t status_reg1 = 0;
	uint8_t status_reg2 = 0;
	uint8_t temp[2];

	status = drv_flash_read_reg_internal(READ_STATUS_CMD, 1, &status_reg1);
	if (status != 0) {
		return status;
	}

	status = drv_flash_read_reg_internal(READ_CFG1_CMD, 1, &status_reg2);
	if (status != 0) {
		return status;
	}

	/* If Quad enable bit already set - nothing to do */
	if (status_reg2 & BIT(QCC730_FLASH_QUAD_MODE145_ENABLE_BIT)) {
		return status;
	}

	/* Set Quad enable bit in status register 2 */
	status_reg2 |= BIT(QCC730_FLASH_QUAD_MODE145_ENABLE_BIT);
	temp[0] = status_reg1;
	temp[1] = status_reg2;

	status = drv_flash_write_reg_internal(dev, WRITE_STATUS_CMD, 2, &temp[0]);
	if (status != 0) {
		return status;
	}

	status = drv_flash_wait_operation_done(dev, READ_STATUS_TIMEOUT, READ_STATUS_POLLING_USEC,
					       OTHER_OPERATION, READ_STATUS_BUSY_MASK, 0);

	return status;
}

/**
   @brief Quad enable mode 2.

   QE is bit 6 of status register 1. It is set via Write status  with one data
   byte where bit 6 is one. It is cleared via Write status  with one data byte
   where bit 6 is zero.

   @return 0 on success or negative error code on failure.
*/
static int drv_flash_enable_quad_mode2(const struct device *dev)
{
	int status = 0;
	uint8_t status_reg = 0;

	status = drv_flash_read_reg_internal(READ_STATUS_CMD, 1, &status_reg);
	if (status != 0) {
		return status;
	}

	/* In mode 2, Bit 6 of status register is used to enable Quad mode */
	if (status_reg & BIT(QCC730_FLASH_QUAD_MODE2_ENABLE_BIT)) {
		return status;
	}

	status_reg |= BIT(QCC730_FLASH_QUAD_MODE2_ENABLE_BIT);
	status = drv_flash_write_reg_internal(dev, WRITE_STATUS_CMD, 1, &status_reg);
	if (status != 0) {
		return status;
	}

	status = drv_flash_wait_operation_done(dev, READ_STATUS_TIMEOUT, READ_STATUS_POLLING_USEC,
					       OTHER_OPERATION, READ_STATUS_BUSY_MASK, 0);

	return status;
}

/**
   @brief Quad enable mode 3.

   QE is bit 7 of status register 2. It is set via Write status register 2
   instruction 3Eh with one data byte where bit 7 is one. It is cleared via
   Write status register 2 instruction 3Eh with one data byte where bit 7 is
   zero. The status register 2 is read using instruction 3Fh.

   @return 0 on success or negative error code on failure.
*/
static int drv_flash_enable_quad_mode3(const struct device *dev)
{
	int status = 0;
	uint8_t status2_reg = 0;

	/* Read 1 byte status  2 register with instruction 3Fh */
	status = drv_flash_read_reg_internal(READ_STATUS_2_CMD, 1, &status2_reg);
	if (status != 0) {
		return status;
	}

	if (status2_reg & BIT(QCC730_FLASH_QUAD_MODE3_ENABLE_BIT)) {
		return status;
	}

	status2_reg |= BIT(QCC730_FLASH_QUAD_MODE3_ENABLE_BIT);
	status = drv_flash_write_reg_internal(dev, WRITE_STATUS_2_CMD, 1, &status2_reg);
	if (status != 0) {
		return status;
	}

	status = drv_flash_wait_operation_done(dev, READ_STATUS_TIMEOUT, READ_STATUS_POLLING_USEC,
					       OTHER_OPERATION, READ_STATUS_BUSY_MASK, 0);

	return status;
}

/**
   @brief Enable flash quad mode.

   @param[in] quad_mode  The quad enable mode for flash, as defined in the
			JEDEC Standard No. 216A Document, Quad Enable
			Requirements in 15th word.

   @return 0 on success or negative error code on failure.
*/
static int drv_flash_enable_quad_mode(const struct device *dev, uint8_t quad_mode)
{
	int status = 0;

	switch (quad_mode) {
	case ENABLE_QUAD_MODE_0:
		/* For mode 0, Device does not have a QE bit. Device detects
		   1-1-4 and 1-4-4 reads based on instruction. */
		break;

	case ENABLE_QUAD_MODE_1:
	case ENABLE_QUAD_MODE_4:
	case ENABLE_QUAD_MODE_5:
		status = drv_flash_enable_quad_mode145(dev);
		break;

	case ENABLE_QUAD_MODE_2:
		status = drv_flash_enable_quad_mode2(dev);
		break;

	case ENABLE_QUAD_MODE_3:
		status = drv_flash_enable_quad_mode3(dev);
		break;

	default:
		LOG_DBG("drv_flash_enable_quad_mode: wrong mode provided: %u", quad_mode);
		status = -EINVAL;
		break;
	}

	return status;
}
#endif

/**
   @brief Initialize the flash controller HW.
    Fermion just support one flash connection. SPI clocking mode is 0 only.

   @return 0 on success or an error code on failure.
*/
static int drv_flash_controller_init()
{
	qspi_master_config_t qspi_cfg;

	memset(&qspi_cfg, 0, sizeof(qspi_master_config_t));

	/* Set clock frequency. */
	qspi_cfg.clk_freq = default_qspi_clock;

	if (drv_qspi_init(&qspi_cfg)) {
		return 0;
	}

	LOG_DBG("drv_flash_controller_init: drv_qspi_init failed");
	return -ENODEV;
}

static int flash_qcc730_qspi_enable(const struct device *dev)
{
	int ret = 0;

#ifdef CONFIG_PM_DEVICE
    qspi_cmd_t qspi_power_up;
	struct flash_qcc730_data *data = dev->data;
#endif

	ret = drv_flash_controller_init();


	if (ret != 0) {
		LOG_ERR("Enable QSPI failed err:%d", ret);
		return ret;
	}
#ifdef CONFIG_PM_DEVICE
	data->qspi_initialized = true;

    (void)drv_qspi_prepare_cmd(&qspi_power_up, POWERUP_OPCODE, 0, 0, QSPI_SDR_1BIT_E, QSPI_SDR_1BIT_E, QSPI_SDR_1BIT_E,
                               false);

    drv_qspi_run_cmd(&qspi_power_up, 0, NULL, 0, QSPI_TRANS_MODE);
#endif

	return ret;
}

#ifdef CONFIG_PM_DEVICE

static int flash_qcc730_qspi_suspend(const struct device *dev)
{
	struct flash_qcc730_data *data = dev->data;
    qspi_cmd_t qspi_power_sleep;

    (void)drv_qspi_prepare_cmd(&qspi_power_sleep, DEEPSLEEP_OPCODE, 0, 0, QSPI_SDR_1BIT_E, QSPI_SDR_1BIT_E,
                               QSPI_SDR_1BIT_E, false);

    drv_qspi_run_cmd(&qspi_power_sleep, 0, NULL, 0, QSPI_TRANS_MODE);

	/* deinit function returns true as a success */
	if (!drv_qspi_deinit()) {
		LOG_ERR("Suspend QSPI failed");
		return -EIO;
	}

	data->qspi_initialized = false;

	return 0;
}

static int flash_qcc730_qspi_pm_action(const struct device *dev, enum pm_device_action action)
{
	int ret;

	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		ret = flash_qcc730_qspi_enable(dev);
		break;
	case PM_DEVICE_ACTION_SUSPEND:
		ret = flash_qcc730_qspi_suspend(dev);
		break;
	default:
		return -ENOTSUP;
	}
	return ret;
}

#endif /* CONFIG_PM_DEVICE */

/**
   @brief Read flash registers.

   @param[in]  reg_opcode  operation code.
   @param[in]  len        The length of register value to be read.
   @param[out] reg_value   The read out value.

   @return 0 on success or an error code on failure.
*/
static int drv_flash_read_reg_internal(uint8_t reg_opcode, uint8_t len, uint8_t *reg_value)
{
	qspi_cmd_t qspi_read_reg;
	int res = 0;

	if (len > 0 && reg_value == NULL) {
		LOG_DBG("drv_flash_read_reg_internal: wrong parameter obtained");
		return -EINVAL;
	}
#if CONFIG_FLASH_QCC730_XIP_MODE
	if (QSPI_TRANS_MODE == QSPI_PIO_MODE_E) {
		drv_qspi_disable_xip_mode();
	}
#endif

	(void)drv_qspi_prepare_cmd(&qspi_read_reg, reg_opcode, 0, 0, QSPI_SDR_1BIT_E,
				   QSPI_SDR_1BIT_E, QSPI_SDR_1BIT_E, false);

	if (!drv_qspi_run_cmd(&qspi_read_reg, 0, reg_value, len, QSPI_TRANS_MODE)) {
		LOG_DBG("drv_flash_read_reg_internal: run_cmd failed");
		res = -ENODEV;
	}

#ifdef CONFIG_FLASH_QCC730_XIP_MODE
	if (QSPI_TRANS_MODE == QSPI_PIO_MODE_E) {
		drv_qspi_restore_xip_mode();
	}
#endif
	return res;
}

/**
   @brief Initialize flash context info.

   @param[in] dev  Flash driver instance
   @param[in] device_id  Id obtained from flash chip

   @return 0 in case of success, negative error code otherwise
*/
static int drv_flash_info_init(const struct device *dev, uint32_t device_id)
{
	struct flash_qcc730_data *data = dev->data;
	int status = 0;
	uint32_t i = 0;
	uint32_t total_flash_dev_variants = flash_get_config_entries_count();
	flash_config_data_t *pflash_device_config = flash_get_config_entries_struct();

	/* Check the list of possible flash devices. If there is no ID
	 * as the one obtained - return -ENODEV error */
	for (i = 0; i < total_flash_dev_variants; i++) {
		if (device_id == pflash_device_config[i].device_id) {
			LOG_INF("found device %x\n", device_id);
			break;
		}
	}
	if (i == total_flash_dev_variants) {
		LOG_ERR("Didn't find any configured device:%x\n", device_id);
		return -ENODEV;
	}

	/* When device has been found - assing it's config from
	 * possible flash variants table */
	memset(&data->flash_ctx_data, 0, sizeof(flash_context_t));
	data->flash_ctx_data.config = pflash_device_config + i;
	if (data->flash_ctx_data.config == NULL) {
		LOG_ERR("Device config is NULL");
		return -ENODEV;
	}

	/* check if the clock rate is the same as device recommended rate
	 * if not -> set the recommended one */
	if (data->flash_ctx_data.config->clk_freq != default_qspi_clock) {
		default_qspi_clock = data->flash_ctx_data.config->clk_freq;
		drv_flash_controller_init();
	}

	return status;
}

/**
   @brief Check flash operation status.

   @param[in] timeout            The total time can be used for status check.
   @param[in] status_polling_usec  The status polling period.
   @param[in] operation          Flash operation, WRITE_OPERATION, or
				 ERASE_OPERATION, or OTHER_OPERATION.
   @param[in] bmask              The bits of the status register to be checked.
   @param[in] status_value        The expected value for the checked status bits.

   @return 0 on success or an error code on failure.
*/
static int drv_flash_wait_operation_done(const struct device *dev, uint32_t timeout,
					 uint32_t status_polling_usec, uint8_t operation,
					 uint8_t bmask, uint8_t status_value)
{
	int ret = -ENODEV;
	uint8_t result = 0;

	if (status_polling_usec == 0 || status_polling_usec > timeout) {
		LOG_DBG("drv_flash_wait_operation_done: wrong parameters obtained"
			" status_polling_usec=%u timeout=%u ", status_polling_usec, timeout);
		return -EINVAL;
	}

	if (IS_ENABLED(CONFIG_DEBUG_COREDUMP) &&
        IS_ENABLED(CONFIG_DEBUG_COREDUMP_BACKEND_FLASH_PARTITION) &&
        k_is_in_isr()) {
        uint32_t remaining = timeout;
        while (remaining) {
            (void)drv_flash_read_reg_internal(READ_STATUS_CMD, 1, &result);

            if ((result & bmask) == status_value) {
                ret = drv_flash_check_error(dev, operation);
                return ret;
            }

            k_busy_wait(status_polling_usec);

            if (remaining >= status_polling_usec) {
                remaining -= status_polling_usec;
            } else {
                remaining = 0;
            }
        }
        return -ETIMEDOUT;
	} else {
		while (timeout) {
			(void)drv_flash_read_reg_internal(READ_STATUS_CMD, 1, &result);

			if ((result & bmask) == status_value) {
				ret = drv_flash_check_error(dev, operation);
				break;
			}

			k_usleep(status_polling_usec);

			timeout -= status_polling_usec;
		}
		return ret;
	}

}

/**
   @brief Flash write enable.

   @return 0 on success or an error code on failure.
*/
static int drv_flash_write_enable(const struct device *dev)
{
	qspi_cmd_t qspi_write_enable;

	(void)drv_qspi_prepare_cmd(&qspi_write_enable, WRITE_ENABLE_CMD, 0, 0, QSPI_SDR_1BIT_E,
				   QSPI_SDR_1BIT_E, QSPI_SDR_1BIT_E, false);

	(void)drv_qspi_run_cmd(&qspi_write_enable, 0, NULL, 0, QSPI_TRANS_MODE);

	return drv_flash_wait_operation_done(dev, READ_STATUS_TIMEOUT, READ_STATUS_POLLING_USEC,
					     OTHER_OPERATION, STATUS_WR_EN_MASK, STATUS_WR_EN_MASK);
}

/**
   @brief Write flash registers.

   @param[in]  reg_opcode  Operation code.
   @param[in]  len         The length of register value to be written.
   @param[out] reg_value   The written value.

   @return 0 on success or an error code on failure.
*/
static int drv_flash_write_reg_internal(const struct device *dev, uint8_t reg_opcode, uint8_t len,
					uint8_t *reg_value)
{
	int ret = 0;
	qspi_cmd_t qspi_write_reg;

	if (len > 0 && reg_value == NULL) {
		LOG_DBG("drv_flash_write_reg_internal: wrong parameter obtained");
		return -EINVAL;
	}

#if CONFIG_FLASH_QCC730_XIP_MODE
	if (QSPI_TRANS_MODE == QSPI_PIO_MODE_E) {
		drv_qspi_disable_xip_mode();
	}
#endif

	(void)drv_qspi_prepare_cmd(&qspi_write_reg, reg_opcode, 0, 0, QSPI_SDR_1BIT_E,
				   QSPI_SDR_1BIT_E, QSPI_SDR_1BIT_E, true);

	ret = drv_flash_write_enable(dev);
	if (ret != 0) {
		LOG_DBG("drv_flash_write_reg_internal: flash_write_enable failed with code %d", ret);
		return ret;
	}

	if (!drv_qspi_run_cmd(&qspi_write_reg, 0, reg_value, len, QSPI_TRANS_MODE)) {
		LOG_DBG("drv_flash_write_reg_internal: drv_qspi_run_cmd failed");
		ret = -ENODEV;
	}

#if CONFIG_FLASH_QCC730_XIP_MODE
	if (QSPI_TRANS_MODE == QSPI_PIO_MODE_E) {
		drv_qspi_restore_xip_mode();
	}
#endif

	return ret;
}

/**
   @brief Clear Specific bits of the flash registers.

   @param[in] reg_read_opcode   Register read operation code.
   @param[in] reg_write_opcode  Register write operation code.
   @param[in] bits_mask        The bits mask of the register that need to be cleared.

   @return 0 on success or an error code on failure.
*/
static int drv_flash_clear_spi_bits(const struct device *dev, uint8_t reg_read_opcode,
				    uint8_t reg_write_opcode, uint8_t bits_mask)
{
	uint8_t read_ret = 0;
	uint8_t read_back = 0;
	int status = 0;

	if (!bits_mask) {
		return status; /* Not set bits_mask, skip */
	}

	/* Get value and check */
	status = drv_flash_read_reg_internal(reg_read_opcode, 1, &read_ret);
	if ((!(read_ret & bits_mask)) || status != 0) {
		return status; /* Bits are cleared already, skip */
	}

	/* Clear bits_mask of value */
	read_ret &= ~bits_mask;
	status = drv_flash_write_reg_internal(dev, reg_write_opcode, 1, &read_ret);
	if (status != 0) {
		return status;
	}

	/* Delay till complete */
	status = drv_flash_wait_operation_done(dev, READ_STATUS_TIMEOUT, READ_STATUS_POLLING_USEC,
					       OTHER_OPERATION, READ_STATUS_BUSY_MASK, 0);
	if (status != 0) {
		LOG_DBG("drv_flash_clear_spi_bits: drv_flash_wait_operation_done failed");
		return status;
	}

	/* Verify the result */
	status = drv_flash_read_reg_internal(reg_read_opcode, 1, &read_back);
	if ((read_back & bits_mask) || (status != 0)) {
		LOG_DBG("drv_flash_clear_spi_bits: drv_flash_read_reg_internal failed");
		return -EIO; /* Bits are not cleared yet, return fail */
	}

	return status;
}

/**
   @brief Clear a list of Specific bits of the flash registers.

   @param[in] reg_read_opcode   Register read operation code list.
   @param[in] reg_write_opcode  Fegister write operation code list.
   @param[in] bits_mask        The list of register bits that need to be cleared.
   @param[in] cnt             The count of the list.

   @return 0 on success or an error code on failure.
*/
static int drv_flash_clear_spi_bits_list(const struct device *dev, const uint8_t *reg_read_opcodes,
					 const uint8_t *reg_write_opcode, const uint8_t *bits_masks,
					 uint8_t cnt)
{
	uint8_t i = 0;
	int status = 0;

	for (i = 0; i < cnt; i++) {
		status = drv_flash_clear_spi_bits(dev, reg_read_opcodes[i], reg_write_opcode[i],
						  bits_masks[i]);
		if (status != 0) {
			return status;
		}
	}

	return status;
}

/**
   @brief Clear flash write protection, so that flash can be written and erased.

   For different flash types, the registers and protect bits may be different.
   They should be handled differently.

   @return 0 on success or an error code on failure.
*/
static int drv_flash_clear_write_protection(const struct device *dev)
{
	struct flash_qcc730_data *data = dev->data;
	uint8_t flash_vid = (uint8_t)(data->flash_ctx_data.config->device_id);
	uint32_t wp_mask = data->flash_ctx_data.config->write_protect_bmask;
	int status = 0;

	if (MANUFACTURER_ID_WINBOND == flash_vid || MANUFACTURER_ID_GD == flash_vid ||
	    MANUFACTURER_ID_GT == flash_vid) {
		/* status  Register Format is as described below for Winbond Flash part.
		   Winbond or GD
		   status -1 Register Format:
		   =======================
		   bit7     bit6    bit5      bit4    bit3    bit2  bit1    bit0
		   SRP      SEC      TB       BP2     BP1     BP0   WEL     BUSY
		   (status  (Sector  (TOP/    (Block protect Bits)  (Write  (Erase/write
		   register protect) bottom                         enable  in progress)
		   protect)          protect)                       latch)

		   status -2 Register Format:
		   =======================
		   bit15    bit14   bit13     bit12   bit11   bit10 bit9    bit8
		   SUS      CMP     LB3       LB2     LB1     (R)   QE      SRL
			    (Coplement                                      (status  register)
			    protect)                                        lock)

		   Expect WriteProtectBmask has {0xFC, 0x41, 0x04} for W25Q32JVZPIQ */
		uint8_t wb_wp_bits[] = {(uint8_t)(wp_mask), (uint8_t)(wp_mask >> 8),
					(uint8_t)(wp_mask >> 16)};
		uint8_t wb_read_cmd_list[] = {READ_STATUS_CMD, WINBOND_READ_STATUS_2_CMD,
					      WINBOND_READ_STATUS_3_CMD};
		uint8_t wb_write_cmd_list[] = {WRITE_STATUS_CMD, WINBOND_WRITE_STATUS_2_CMD,
					       WINBOND_WRITE_STATUS_3_CMD};
		status = drv_flash_clear_spi_bits_list(
			dev, (const uint8_t *)wb_read_cmd_list, (const uint8_t *)wb_write_cmd_list,
			(const uint8_t *)wb_wp_bits, sizeof(wb_wp_bits));
	} else if (MANUFACTURER_ID_MACRONIX == flash_vid || MANUFACTURER_ID_ISSI == flash_vid) {
		/* WORKAROUND: On some Macronix parts, the block write protection,
		non-volatile bits of status register i.e. BP3-BP0 (bit5-bit2) are
		spuriously set thereby causing erase/write operation on Flash to fail.

		This is a temporary WORKAROUND to recover from this situation until
		the actual reason of how these bits are set is uncovered.
		TODO: Need to revisit once the actual problem is root caused.

		status  Register Format is as described below for Macronix Flash part.
		Macronix status  Register Format:
		=======================
		bit7     bit6    bit5    bit4    bit3    bit2    bit1    bit0
		SRWD     QE      BP3     BP2     BP1     BP0     WEL     WIP
		(status  (Quad   (---"---Level of                (Write  (Write
		register Enable)         Protected Block--"---)  enable  in
		write                                            latch)  progress)
		protect)

		1=status 1=Quad                                  1=write 1=write
		register Enable                                  enable  operation
		write    0=not                                   0=not   0=not in
		disable  Quad                                    write   write
		    Enable                                  enable  operation

		(--------"-------Non-volatile bits----"-------)  (-"-Volatile bits-"-)

		reserved_3 for Macronix parts is defaulted to 0xBC in OEM modifiable
		flash_config stored in AON region and considered as a mask to detect
		whether these bits are set. If set then auto-clear these bits.

		Expect reserved_3 has {0xBC, 0x00, 0x00} for MX25 */
		status = drv_flash_clear_spi_bits(dev, READ_STATUS_CMD, WRITE_STATUS_CMD,
						  (uint8_t)(wp_mask));
	}

	return status;
}

/**
   @brief Check flash operation error

   @param[in] operation_type  Flash operation type, WRITE_OPERATION,
			     ERASE_OPERATION, or OTHER_OPERATION.

   @return 0 on success operation or an error code on failure.
*/
static int drv_flash_check_error(const struct device *dev, uint8_t operation_type)
{
	struct flash_qcc730_data *data = dev->data;
	uint8_t status_mask = 0;
	uint8_t err_status_register = 0;
	uint8_t qspi_status = 0;
	int ret = 0;

	switch (operation_type) {
	case WRITE_OPERATION:
		status_mask = data->flash_ctx_data.config->write_err_bmsk;
		err_status_register = data->flash_ctx_data.config->write_err_status_reg;
		break;

	case ERASE_OPERATION:
		status_mask = data->flash_ctx_data.config->erase_err_bmsk;
		err_status_register = data->flash_ctx_data.config->erase_err_status_reg;
		break;

	default:
		break;
	}

	if (err_status_register == 0) {
		return ret;
	}

	ret = drv_flash_read_reg_internal(err_status_register, 1, &qspi_status);
	if ((ret == 0) && (qspi_status & status_mask)) {
		ret = -EIO;
	}

	return ret;
}

static bool area_is_valid(const struct device *dev, off_t offset, size_t size)
{
	struct flash_qcc730_data *data = dev->data;
	uint32_t flash_size = data->flash_ctx_data.config->density_in_blocks * BLOCK_SIZE_IN_BYTES;

	/* Check if size and offset are correct
	 * 1. Offset has to be non negative
	 * 2. Offset can not be bigger than memory length
	 * 3. Memory size without offset can not be less than size to be manipulated
	 * 4. Size to be manipulated has to be positive
	 * */
	if ((offset < 0) || (offset >= flash_size) || (flash_size - offset) < size || size <= 0) {
		return false;
	}

	return true;
}

int flash_qcc730_qspi_nor_erase(const struct device *dev, off_t offset, size_t size)
{
	struct flash_qcc730_data *data = dev->data;
	uint32_t flash_size = data->flash_ctx_data.config->density_in_blocks * BLOCK_SIZE_IN_BYTES;
	uint8_t opcode = 0;
	qspi_cmd_t qspi_erase_cmd;
	uint8_t addr_bytes_num = data->flash_ctx_data.config->addr_bytes;
	uint32_t erase_timeout = ERASE_TIMEOUT;
	uint32_t erase_polling = ERASE_STATUS_POLLING_MSEC;
	uint32_t address = offset;
	uint32_t size_of_chunk = 0;
	uint16_t op_cnt = 1;
	int ret = 0;

	if (!area_is_valid(dev, offset, size)) {
		LOG_DBG("flash_qcc730_qspi_nor_erase: provided area is invalid"
			" offset=%ld size=%u", offset, size);
		return -EINVAL;
	}

#ifdef CONFIG_PM_DEVICE
	if (!data->qspi_initialized) {
		LOG_ERR("Driver is suspended ");
		return -EBUSY;
	}
#endif

#if defined(CONFIG_MULTITHREADING)
	k_sem_take(&data->sem, FLASH_SEM_TIMEOUT);
#endif

#if CONFIG_FLASH_QCC730_XIP_MODE
	if (QSPI_TRANS_MODE == QSPI_PIO_MODE_E) {
		drv_qspi_disable_xip_mode();
	}
#endif
	if (size == flash_size) {
		/* Whole chip erase*/
		opcode = data->flash_ctx_data.config->chip_erase_opcode;
		addr_bytes_num = 0;
		erase_timeout = CHIP_ERASE_TIMEOUT;
		erase_polling = CHIP_ERASE_STATUS_POLLING_MSEC;
		size_of_chunk = flash_size;
		op_cnt = 1;
	} else if (size % SIZE_64K_BYTES == 0) {
		/* Block erase (64kB) */
		opcode = data->flash_ctx_data.config->bulk_erase_opcode;
		size_of_chunk = SIZE_64K_BYTES;
		op_cnt = MAX(size / SIZE_64K_BYTES, 1);
	} else {
		/* Sector erase (4kB) */
		opcode = data->flash_ctx_data.config->erase_4kb_opcode;
		size_of_chunk = BLOCK_SIZE_IN_BYTES;
		op_cnt = MAX(size / BLOCK_SIZE_IN_BYTES, 1);
	}

	(void)drv_qspi_prepare_cmd(&qspi_erase_cmd, opcode, addr_bytes_num, 0, QSPI_SDR_1BIT_E,
				   QSPI_SDR_1BIT_E, QSPI_SDR_1BIT_E, false);

#if CONFIG_FLASH_QCC730_XIP_MODE
	/* Single the HW write operation is ongoing. Needed for XIP. */
	if (data->flash_ctx_data.config->suspend_program_opcode > 0 &&
	    data->flash_ctx_data.config->resume_program_opcode > 0) {
		(void)drv_qspi_xip_set_pe_state(true);
		(void)drv_qspi_xip_config_suspend_resume(
			data->flash_ctx_data.config->suspend_erase_delay_in_us,
			data->flash_ctx_data.config->suspend_erase_opcode,
			data->flash_ctx_data.config->resume_erase_delay_in_us,
			data->flash_ctx_data.config->resume_erase_opcode);
	}
#endif

	while (op_cnt) {
		ret = drv_flash_write_enable(dev);
		if (ret != 0) {
			ret = -ENODEV;
			break;
		}

		drv_qspi_run_cmd(&qspi_erase_cmd, address, NULL, 0, QSPI_TRANS_MODE);

		ret = drv_flash_wait_operation_done(dev, erase_timeout, erase_polling * 1000,
						    ERASE_OPERATION, PROG_ERASE_WRITE_BUSY_BMSK, 0);
		if (ret != 0) {
			ret = -ECOMM;
			break;
		}

		address += size_of_chunk;
		op_cnt--;
	}

#if CONFIG_FLASH_QCC730_XIP_MODE
	(void)drv_qspi_xip_set_pe_state(false);
	if (QSPI_TRANS_MODE == QSPI_PIO_MODE_E) {
		drv_qspi_restore_xip_mode();
	}
#endif

#if defined(CONFIG_MULTITHREADING)
	k_sem_give(&data->sem);
#endif

	return 0;
}

int flash_qcc730_qspi_nor_write(const struct device *dev, off_t offset, const void *write_buff,
				size_t len)
{
	qspi_cmd_t qspi_page_write_cmd;
	struct flash_qcc730_data *data = dev->data;
	uint32_t transfer_size = 0;
	uint8_t *buff_ptr = (uint8_t *)(write_buff);
	int ret = 0;
	int leftover = 0;

	if (!area_is_valid(dev, offset, len)) {
		LOG_DBG("flash_qcc730_qspi_nor_write: provided area is invalid"
			" offset=%ld len=%u", offset, len);
		return -EINVAL;
	}

	if (!buff_ptr) {
		return -EINVAL;
	}

#ifdef CONFIG_PM_DEVICE
	if (!data->qspi_initialized) {
		LOG_ERR("Driver is suspended ");
		return -EBUSY;
	}
#endif

#if defined(CONFIG_MULTITHREADING)
	k_sem_take(&data->sem, FLASH_SEM_TIMEOUT);
#endif

#if CONFIG_FLASH_QCC730_XIP_MODE
	if (QSPI_TRANS_MODE == QSPI_PIO_MODE_E) {
		drv_qspi_disable_xip_mode();
	}
#endif

	(void)drv_qspi_prepare_cmd(&qspi_page_write_cmd, data->flash_ctx_data.config->write_opcode,
				   data->flash_ctx_data.config->addr_bytes, 0,
				   (qspi_mode_t)data->flash_ctx_data.config->write_cmd_mode,
				   (qspi_mode_t)data->flash_ctx_data.config->write_addr_mode,
				   (qspi_mode_t)data->flash_ctx_data.config->write_data_mode, true);

#if CONFIG_FLASH_QCC730_XIP_MODE
	/* Signle the HW write operation is ongoing. Needed for XIP. */
	if (data->flash_ctx_data.config->suspend_program_opcode > 0 &&
	    data->flash_ctx_data.config->resume_program_opcode > 0) {
		(void)drv_qspi_xip_set_pe_state(true);
		(void)drv_qspi_xip_config_suspend_resume(
			data->flash_ctx_data.config->suspend_program_delay_in_us,
			data->flash_ctx_data.config->suspend_program_opcode,
			data->flash_ctx_data.config->resume_program_delay_in_us,
			data->flash_ctx_data.config->resume_program_opcode);
	}
#endif

	while (len) {
		leftover = offset % PAGE_SIZE_IN_BYTES;

		if (leftover) {
			transfer_size = PAGE_SIZE_IN_BYTES - leftover;
			if (transfer_size > len) {
				transfer_size = len;
			}
		} else {
			transfer_size = (len > PAGE_SIZE_IN_BYTES) ? (PAGE_SIZE_IN_BYTES) : (len);
		}

		ret = drv_flash_write_enable(dev);
		if (ret < 0) {
			break;
		}

		(void)drv_qspi_run_cmd(&qspi_page_write_cmd, offset, buff_ptr, transfer_size,
				       QSPI_TRANS_MODE);

		ret = drv_flash_wait_operation_done(dev, WRITE_TIMEOUT, WRITE_STATUS_POLLING_USEC,
						    WRITE_OPERATION, PROG_ERASE_WRITE_BUSY_BMSK, 0);
		if (ret != 0) {
			break;
		}

		offset += transfer_size;
		buff_ptr += transfer_size;
		len -= transfer_size;
	}

#if CONFIG_FLASH_QCC730_XIP_MODE
	(void)drv_qspi_xip_set_pe_state(false);
	if (QSPI_TRANS_MODE == QSPI_PIO_MODE_E) {
		drv_qspi_restore_xip_mode();
	}
#endif

#if defined(CONFIG_MULTITHREADING)
	k_sem_give(&data->sem);
#endif

	return ret;
}

int flash_qcc730_qspi_nor_read(const struct device *dev, off_t offset, void *read_buff, size_t len)
{
	struct flash_qcc730_data *data = dev->data;
	qspi_cmd_t qspi_read_cmd;

#ifdef CONFIG_PM_DEVICE
	if (!data->qspi_initialized) {
		LOG_ERR("Driver is suspended ");
		return -EBUSY;
	}
#endif

	if (!len) {
		return 0;
	}

	if (!area_is_valid(dev, offset, len)) {
		LOG_DBG("flash_qcc730_qspi_nor_read: provided area is invalid"
			" offset=%ld len=%u", offset, len);
		return -EINVAL;
	}

	if (!read_buff) {
		LOG_DBG("flash_qcc730_qspi_nor_read: read_buff == NULL");
		return -EINVAL;
	}

#if defined(CONFIG_MULTITHREADING)
	k_sem_take(&data->sem, FLASH_SEM_TIMEOUT);
#endif

#if CONFIG_FLASH_QCC730_XIP_MODE
	if (QSPI_TRANS_MODE == QSPI_PIO_MODE_E) {
		drv_qspi_disable_xip_mode();
	}
#endif

	(void)drv_qspi_prepare_cmd(&qspi_read_cmd, data->flash_ctx_data.config->read_opcode,
				   data->flash_ctx_data.config->addr_bytes,
				   data->flash_ctx_data.config->read_wait_state,
				   (qspi_mode_t)data->flash_ctx_data.config->read_cmd_mode,
				   (qspi_mode_t)data->flash_ctx_data.config->read_addr_mode,
				   (qspi_mode_t)data->flash_ctx_data.config->read_data_mode, false);

	if (!drv_qspi_run_cmd(&qspi_read_cmd, offset, read_buff, len, QSPI_TRANS_MODE)) {
#if CONFIG_FLASH_QCC730_XIP_MODE
		if (QSPI_TRANS_MODE == QSPI_PIO_MODE_E) {
			(void)drv_qspi_restore_xip_mode();
		}
#endif
		LOG_DBG("flash_qcc730_qspi_nor_read: sending read command failed");

		return -ECOMM;
	}

#if CONFIG_FLASH_QCC730_XIP_MODE
	if (QSPI_TRANS_MODE == QSPI_PIO_MODE_E) {
		(void)drv_qspi_restore_xip_mode();
	}
#endif

#if defined(CONFIG_MULTITHREADING)
	k_sem_give(&data->sem);
#endif

	return 0;
}

const struct flash_parameters *flash_qcc730_qspi_nor_get_parameters(const struct device *dev)
{
	ARG_UNUSED(dev);

	return &flash_qcc730_parameters;
}

int flash_qcc730_qspi_nor_get_size(const struct device *dev, uint64_t *size)
{
	struct flash_qcc730_data *data = dev->data;

	*size = data->flash_ctx_data.config->density_in_blocks * BLOCK_SIZE_IN_BYTES;

	return 0;
}

static int flash_qcc730_qspi_nor_init(const struct device *dev)
{
	struct flash_qcc730_data *data = dev->data;
	const struct flash_qcc730_config *cfg = dev->config;
	uint32_t device_id = 0;
	int ret = 0;

	LOG_DBG("\nflash_qcc730_qspi_nor is initializing\n");

#if defined(CONFIG_MULTITHREADING)
	k_sem_init(&data->sem, 1, 1);
#endif

	/* Initialize QSPI driver. */
	ret = flash_qcc730_qspi_enable(dev);
	if (ret != 0) {
		LOG_DBG("flash_qcc730_qspi_nor_init: flash controller init failed");
		return -ENODEV;
	}

	/* Check device id. */
	ret = drv_flash_read_reg_internal(READ_IDENTIFICATION_CMD, 3, (uint8_t *)&device_id);
	if (ret != 0) {
		return -ENODEV;
	}

	if (device_id != cfg->device_id) {
		LOG_ERR("FLASH device with ID: %x was not found!\n", cfg->device_id);
		return -ENODEV;
	}

	/* Initialize flash context info */
	ret = drv_flash_info_init(dev, device_id);
	if (ret != 0) {
		return -ENODEV;
	}

	/* Clear write protection */
	ret = drv_flash_clear_write_protection(dev);
	if (ret != 0) {
		LOG_DBG("flash_qcc730_qspi_nor_init: clearing write protection failed");
		return -ENODEV;
	}

	/* Check rw mode. */
	if (!VALID_RW_MODE(data->flash_ctx_data.config->read_cmd_mode) ||
	    !VALID_RW_MODE(data->flash_ctx_data.config->read_addr_mode) ||
	    !VALID_RW_MODE(data->flash_ctx_data.config->read_data_mode) ||
	    !VALID_RW_MODE(data->flash_ctx_data.config->write_cmd_mode) ||
	    !VALID_RW_MODE(data->flash_ctx_data.config->write_addr_mode) ||
	    !VALID_RW_MODE(data->flash_ctx_data.config->write_data_mode)) {
		LOG_DBG("flash_qcc730_qspi_nor_init: rw mode invalid");
		return -EINVAL;
	}

#if CONFIG_FLASH_QCC730_QSPI_QUAD_MODE
	/* Set quad mode */
	if (IS_QUAD_MODE(data->flash_ctx_data.config->read_cmd_mode) ||
	    IS_QUAD_MODE(data->flash_ctx_data.config->read_addr_mode) ||
	    IS_QUAD_MODE(data->flash_ctx_data.config->read_data_mode) ||
	    IS_QUAD_MODE(data->flash_ctx_data.config->write_cmd_mode) ||
	    IS_QUAD_MODE(data->flash_ctx_data.config->write_addr_mode) ||
	    IS_QUAD_MODE(data->flash_ctx_data.config->write_data_mode)) {
		ret = drv_flash_enable_quad_mode(dev,
						 data->flash_ctx_data.config->quad_enable_mode);
		if (ret != 0) {
			LOG_DBG("flash_qcc730_qspi_nor_init: enabling quad mode failed");
			return -EOPNOTSUPP;
		}
	}
#endif

	/* Set address mode */
	if ((data->flash_ctx_data.config->addr_bytes == 4) &&
	    (data->flash_ctx_data.config->density_in_blocks * BLOCK_SIZE_IN_BYTES >
	     FLASH_16MB_IN_BYTES)) {
		ret = drv_flash_write_reg_internal(dev, ENTER_4B_ADDR_CMD, 0, NULL);
		if (ret != 0) {
			return -EOPNOTSUPP;
		}
	}

	LOG_DBG("\n FLASH init success \n");

	return 0;
}

#if CONFIG_FLASH_PAGE_LAYOUT
void flash_qcc730_qspi_nor_page_layout(const struct device *dev,
				       const struct flash_pages_layout **layout,
				       size_t *layout_size)
{
	const struct flash_qcc730_config *cfg = dev->config;

	*layout = &cfg->page_layout;
	*layout_size = 1;
}
#endif

static DEVICE_API(flash, flash_qcc730_qspi_nor_api) = {
	.erase = flash_qcc730_qspi_nor_erase,
	.write = flash_qcc730_qspi_nor_write,
	.read = flash_qcc730_qspi_nor_read,
	.get_parameters = flash_qcc730_qspi_nor_get_parameters,
	.get_size = flash_qcc730_qspi_nor_get_size,
#if CONFIG_FLASH_PAGE_LAYOUT
	.page_layout = flash_qcc730_qspi_nor_page_layout,
#endif
};

#define DEFINE_FLASH_QCC730(n)                                                                     \
                                                                                                   \
	static struct flash_qcc730_data flash_qspi_data_##n = {};                                  \
                                                                                                   \
	static const struct flash_qcc730_config flash_qspi_config_##n = {                          \
		.device_id = DT_INST_PROP(n, device_id),                                           \
		.size = DT_INST_REG_SIZE(n),                                                       \
		.page_layout =                                                                     \
			{                                                                          \
				.pages_count = DT_INST_REG_SIZE(n) / BLOCK_SIZE_IN_BYTES,          \
				.pages_size = BLOCK_SIZE_IN_BYTES,                                 \
			},                                                                         \
	};                                                                                         \
                                                                                                   \
	PM_DEVICE_DT_INST_DEFINE(n, flash_qcc730_qspi_pm_action);                                  \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, flash_qcc730_qspi_nor_init, PM_DEVICE_DT_INST_GET(n),             \
			      &flash_qspi_data_##n,                                                \
			      &flash_qspi_config_##n,                                              \
			      POST_KERNEL, CONFIG_FLASH_INIT_PRIORITY,                             \
			      &flash_qcc730_qspi_nor_api);

DT_INST_FOREACH_STATUS_OKAY(DEFINE_FLASH_QCC730)
