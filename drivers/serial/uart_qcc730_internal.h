/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Driver for UART port on STM32 family processor.
 *
 */

#ifndef ZEPHYR_DRIVERS_SERIAL_UART_QCC730_INTERNAL_H_
#define ZEPHYR_DRIVERS_SERIAL_UART_QCC730_INTERNAL_H_

#include <zephyr/drivers/uart.h>
#include <uart_hal.h>

#define UART_DUMP_CONF					BIT(0)
#define UART_DUMP_REG					BIT(1)
#define UART_DUMP_TX					BIT(2)
#define UART_DUMP_RX					BIT(3)
#define UART_DUMP_IRQS					BIT(4)
#define UART_DUMP_STATS					BIT(5)
#define UART_DUMP_ALL					(UART_DUMP_CONF|UART_DUMP_REG|UART_DUMP_TX|UART_DUMP_RX|UART_DUMP_STATS)

#define UART_TX_BUFF_SIZE			32
#define UART_RX_BUFF_SIZE			32

typedef enum {
	UART_TX_DIR,
	UART_RX_DIR,
} uart_dir;

typedef enum {
	UART_SUCCESS,
	UART_ERROR,
	UART_ERROR_NULL_PTR,
	UART_ERROR_INVALID_PARAM,
	UART_ERROR_CFG_PARAM,
	UART_ERROR_BAUDRATE_CFG,
	UART_ERROR_SEND_BUSY,
	UART_ERROR_RECV_BUSY,
	UART_ERROR_TX_ENQUEUE_SEM_SYNC,
	UART_ERROR_TX_ENQUEUE_FULL,
	UART_ERROR_TX_DEQUEUE_EMPTY,
	UART_ERROR_RX_ENQUEUE_FULL,
	UART_ERROR_RX_DEQUEUE_SEM_SYNC,
	UART_ERROR_RX_DEQUEUE_EMPTY,
	UART_ERROR_TRANSFER_TIMEOUT,
	UART_ERROR_INPUT_FIFO_UNDER_RUN,
	UART_ERROR_INPUT_FIFO_OVER_RUN,
	UART_ERROR_OUTPUT_FIFO_UNDER_RUN,
	UART_ERROR_OUTPUT_FIFO_OVER_RUN,
	UART_ERROR_TRANSFER_FORCE_TERMINATED,
	UART_ERROR_BUS_CLK_CFG_FAIL,
	UART_ERROR_BUS_GPIO_ENABLE_FAIL,
	UART_ERROR_CANCEL_TRANSFER_FAIL,
	UART_ERROR_BOOTSTRAP_CFG_FAIL,
	UART_ERROR_DEVICE_STATE,
} uart_status;

typedef struct {
	uint8_t *ring;
	uint32_t size;
	uint32_t wr_idx;
	uint32_t rd_idx;
	uint32_t timeout;
	uint32_t thres;
	uint32_t full_cnt;
	//nt_osal_semaphore_handle_t	sync_sem;
} uart_xfr;

typedef enum {
	UART_INSTANCE_0,
	UART_INSTANCE_MAX,
} uart_instance;

typedef struct {
	uint8_t	 line_status;
	uint8_t  rx_avail;
	uint8_t	 char_timeout;
	uint8_t  tx_empty;
	uint8_t  modem_status;
	uint8_t  busy_detect;
	uint8_t	 no_pending;
} uart_irqs;

typedef struct {
	uint32_t rx_irq_num;
	uint32_t tx_irq_num;
} uart_stats;

/* device config */
struct uart_qcc730_device_config {
	uart_hal		*hal;		
	struct uart_config uart_cfg;
};

/* driver data */
struct uart_qcc730_device_data {
	uint32_t		state;
	uart_xfr		tx;
	uart_xfr		rx;
	uart_irqs		irqs;
	uart_stats		stats;
};

#endif	/* ZEPHYR_DRIVERS_SERIAL_UART_QCC730_INTERNAL_H_ */
