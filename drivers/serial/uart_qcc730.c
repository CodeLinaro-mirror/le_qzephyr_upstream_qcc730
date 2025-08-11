/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qcom_uart_qcc730

/**
 * @brief Driver for UART port on STM32 family processor.
 * @note  LPUART and U(S)ART have the same base and
 *        majority of operations are performed the same way.
 *        Please validate for newly added series.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/sys/__assert.h>
#include <soc.h>
#include <zephyr/init.h>

#include <zephyr/drivers/serial/uart_qcc730.h>
#include "uart_qcc730_internal.h"

#include <zephyr/logging/log.h>
#include <zephyr/irq.h>

#include <qlib_early_printk.h>
#include <qlib_util.h>

void nt_myputchar(uint32_t ch);
void early_printk(const char *fmt, ...);

static uint8_t tx_buf[UART_TX_BUFF_SIZE];
static uint8_t rx_buf[UART_RX_BUFF_SIZE];

static void uart_dump(const struct device *dev, uint8_t dump)
{
    struct uart_qcc730_device_data *data = (struct uart_qcc730_device_data *)dev->data;
    struct uart_qcc730_device_config *cfg = (struct uart_qcc730_device_config *)dev->config;
    uint32_t value;

    if (dev == NULL)
        return;

    if (dump & UART_DUMP_CONF) {
        struct uart_config *config = &cfg->uart_cfg;
        early_printk("UART device config dump\r\n");
        early_printk("	baudrate:%u\r\n", (unsigned int)(config->baudrate));
        early_printk("	parity:%u\r\n", (unsigned int)(config->parity));
        early_printk("	data_bits:%u\r\n", (unsigned int)(config->data_bits));
        early_printk("	stop_bits:%u\r\n", (unsigned int)(config->stop_bits));
        early_printk("	flow_ctrl:%u\r\n", (unsigned int)(config->flow_ctrl));
    }

    if (dump & UART_DUMP_REG) {
        uart_hal *hal = cfg->hal;
        early_printk("UART device regs dump\r\n");
        value = hal->UART_UART_DLH.reg;
        early_printk("UART DLH/IER(0x1233804):0x%x\r\n", (unsigned int)(value));
        value = hal->UART_UART_IIR.reg;
        early_printk("UART FCR/IIR(0x1233808):0x%x\r\n", (unsigned int)(value));
        value = hal->UART_UART_LCR.reg;
        early_printk("UART LCR(0x123380C):0x%x\r\n", (unsigned int)(value));
        value = hal->UART_UART_MCR.reg;
        early_printk("UART MCR(0x1233810):0x%x\r\n", (unsigned int)(value));
        value = hal->UART_UART_LSR.reg;
        early_printk("UART LSR(0x1233814):0x%x\r\n", (unsigned int)(value));
        value = hal->UART_UART_MSR.reg;
        early_printk("UART MSR(0x1233818):0x%x\r\n", (unsigned int)(value));
        value = hal->UART_UART_USR.reg;
        early_printk("UART USR(0x123387C):0x%x\r\n", (unsigned int)(value));
    }

    if (dump & UART_DUMP_TX) {
        uart_xfr *tx = &data->tx;
        early_printk("UART data Tx dump\r\n");
        early_printk("	size:%u\r\n", (unsigned int)(tx->size));
    }

    if (dump & UART_DUMP_RX) {
        uart_xfr *rx = &data->rx;
        early_printk("UART data Rx dump\r\n");
        early_printk("	size:%u\r\n", (unsigned int)(rx->size));
    }

    if (dump & UART_DUMP_IRQS) {
        uart_irqs *irqs = &data->irqs;
        early_printk("UART data irqs dump\r\n");
        early_printk("	rx_avail:%u\r\n", (unsigned int)(irqs->rx_avail));
        early_printk("	tx_empty:%u\r\n", (unsigned int)(irqs->tx_empty));
        early_printk("	no_pending:%u\r\n", (unsigned int)(irqs->no_pending));
    }

    if (dump & UART_DUMP_STATS) {
        early_printk("UART data stats dump\r\n");
        uart_stats *stats = &data->stats;
        early_printk("	rx_irq_num:%u\r\n", (unsigned int)(stats->rx_irq_num));
        early_printk("	tx_irq_num:%u\r\n", (unsigned int)(stats->tx_irq_num));
    }

    return;
}

static void uart_irqs_get(uart_irqs *irqs, uart_hal *hal)
{
    uint32_t irq_raw, status_raw;

    if (irqs == NULL || hal == NULL)
        return;

    irq_raw = uart_hal_intr_get(hal);

    switch (irq_raw & IIR_IID_MASK) {
    case IIR_NO_INTR_PENDING:
        irqs->no_pending = 1;
#ifndef UART_FIFO_MODE
        /* Hardware issue, IID is no_intr_pending when data arrival */
        irqs->rx_avail = 1;
#endif
        break;
    case IIR_THR_EMPTY:
        irqs->tx_empty = 1;
        break;
    case IIR_LINE_STATUS:
        irqs->line_status = 1;
        // Reading line status register to reset the irq;
        status_raw = uart_hal_line_status_get(hal);
        break;
    case IIR_RX_DATA_AVAIL:
        irqs->rx_avail = 1;
        break;
    case IIR_CHAR_TIMEOUT:
        irqs->char_timeout = 1;
        break;
    case IIR_MODEM_STATUS:
        irqs->modem_status = 1;
        break;
    case IIR_BUSY_DETECT:
        irqs->busy_detect = 1;
        break;
    default:
        break;
    }

    (void)status_raw;

    return;
}

// This function only called from ISR
static uart_status uart_rx_ring_enqueue(uart_xfr *xfr, uint8_t data)
{
    uint32_t wr_next;

    if (xfr == NULL)
        return UART_ERROR_NULL_PTR;

    wr_next = xfr->wr_idx + 1;

    if (wr_next >= xfr->size)
        wr_next = 0;

    if (wr_next == xfr->rd_idx) {
        xfr->full_cnt++;
        return UART_ERROR_RX_ENQUEUE_FULL;
    }

    xfr->ring[xfr->wr_idx] = data;
    xfr->wr_idx = wr_next;

    return UART_SUCCESS;
}

static uart_status uart_rx_ring_dequeue(uart_xfr *xfr, uint8_t *data)
{
    uint32_t key = 0;

    if (xfr == NULL || data == NULL)
        return UART_ERROR_NULL_PTR;

    key = irq_lock();
    if (xfr->rd_idx == xfr->wr_idx) {
        irq_unlock(key);
        return UART_ERROR_RX_DEQUEUE_EMPTY;
    }

    *data = xfr->ring[xfr->rd_idx++];

    if (xfr->rd_idx >= xfr->size)
        xfr->rd_idx = 0;

    irq_unlock(key);
    return UART_SUCCESS;
}

static void uart_qcc730_isr(const struct device *dev)
{
    struct uart_qcc730_device_data *data = (struct uart_qcc730_device_data *)dev->data;
    struct uart_qcc730_device_config *cfg = (struct uart_qcc730_device_config *)dev->config;

    uart_hal *hal = cfg->hal;
    uart_xfr *xfr_rx = &data->rx;
    uart_irqs *irqs = &data->irqs;
    uart_stats *stats = &data->stats;
    uint8_t ch = 0;

    memset(irqs, 0, sizeof(uart_irqs));
    uart_irqs_get(irqs, hal);
    if (irqs->rx_avail) {
        stats->rx_irq_num++;

        /* Hardware issue that LSR data ready bit not work */
        /* while (uart_hal_intr_rx_ready(hal)) {
            if (uart_fifo_read(hal, &data, 1) == 0)
                break;

            if (uart_rx_ring_enqueue(xfr_rx, data) != UART_SUCCESS)
                break;
        } */
        ch = uart_hal_rx_read(hal);
        uart_rx_ring_enqueue(xfr_rx, ch);
    }
    if (irqs->tx_empty) {
        stats->tx_irq_num++;
    }
}

static uart_status uart_xfr_init(uart_xfr *xfr, uint8_t *buf, uint32_t size, uint32_t timeout, uart_dir dir,
                                 uint32_t thres)
{
    ARG_UNUSED(timeout);
    ARG_UNUSED(dir);

    if (xfr == NULL) {
        early_printk("uart_xfer_init param invalid\n");
        return UART_ERROR_NULL_PTR;
    }

    xfr->ring = buf;
    xfr->size = size;
    xfr->rd_idx = 0;
    xfr->wr_idx = 0;
    xfr->thres = thres;

    return UART_SUCCESS;
}

static int uart_qcc730_poll_in(const struct device *dev, unsigned char *c)
{
    struct uart_qcc730_device_data *data = (struct uart_qcc730_device_data *)dev->data;

    return uart_rx_ring_dequeue(&data->rx, c);
}

static void uart_qcc730_poll_out(const struct device *dev, unsigned char c)
{
    struct uart_qcc730_device_config *cfg = (struct uart_qcc730_device_config *)dev->config;
    uart_hal_poll_out(cfg->hal, c);
}

static const struct uart_driver_api uart_qcc730_driver_api = {
    .poll_in = uart_qcc730_poll_in,
    .poll_out = uart_qcc730_poll_out,
};

static void uart_qcc730_irq_config_func(const struct device *dev)
{
    ARG_UNUSED(dev);
    IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority), uart_qcc730_isr, DEVICE_DT_INST_GET(0), 0);
    irq_enable(DT_INST_IRQN(0));
}

/**
 * @brief Initialize UART channel
 *
 * This routine is called to reset the chip in a quiescent state.
 * It is assumed that this function is called only once per UART.
 *
 * @param dev UART device struct
 *
 * @return 0
 */
static int uart_qcc730_init(const struct device *dev)
{
    struct uart_qcc730_device_data *data = (struct uart_qcc730_device_data *)dev->data;
    struct uart_qcc730_device_config *cfg = (struct uart_qcc730_device_config *)dev->config;

    early_printk("\r\n");
    memset(data, 0, sizeof(struct uart_qcc730_device_data));
    uart_xfr_init(&data->tx, tx_buf, sizeof(tx_buf), 0, UART_TX_DIR, (UART_HW_FIFO_SIZE / 2));
    uart_xfr_init(&data->rx, rx_buf, sizeof(rx_buf), 0, UART_RX_DIR, (UART_HW_FIFO_SIZE / 2));
    uart_qcc730_irq_config_func(dev);
    uart_hal_enable_intr_rx(cfg->hal);
    // uart_dump(dev, UART_DUMP_ALL);

    return 0;
}

#define UART_QCC730_INIT(n)                                                                                            \
    static struct uart_qcc730_device_data uart_qcc730_dev_data_##n = {};                                               \
                                                                                                                       \
    static const struct uart_qcc730_device_config uart_qcc730_dev_cfg_##n = {                                          \
        .hal = (uart_hal *)DT_INST_REG_ADDR(n),                                                                        \
        .uart_cfg =                                                                                                    \
            {                                                                                                          \
                .baudrate = DT_INST_PROP(n, current_speed),                                                            \
                .parity = DT_INST_ENUM_IDX_OR(n, parity, UART_CFG_PARITY_NONE),                                        \
                .stop_bits = DT_INST_ENUM_IDX_OR(n, stop_bits, UART_CFG_STOP_BITS_1),                                  \
                .data_bits = DT_INST_ENUM_IDX_OR(n, data_bits, UART_CFG_DATA_BITS_8),                                  \
                .flow_ctrl = DT_INST_PROP(n, hw_flow_control) ? UART_CFG_FLOW_CTRL_RTS_CTS : UART_CFG_FLOW_CTRL_NONE,  \
            },                                                                                                         \
    };                                                                                                                 \
                                                                                                                       \
    DEVICE_DT_INST_DEFINE(n, uart_qcc730_init, NULL, &uart_qcc730_dev_data_##n, &uart_qcc730_dev_cfg_##n,              \
                          PRE_KERNEL_1, CONFIG_SERIAL_INIT_PRIORITY, &uart_qcc730_driver_api);

DT_INST_FOREACH_STATUS_OKAY(UART_QCC730_INIT)

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

static int cmd_qcc730_test1(const struct shell *ctx, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_print(ctx, "cmd_uart_conf");
    return 0;
}

static int cmd_qcc730_test2(const struct shell *ctx, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_print(ctx, "cmd_uart_transceive");
    return 0;
}

#if 0
#define NT_LOG_LVL_INFO 0
/*! @warning condition priority. */
#define NT_LOG_LVL_WARN 1
/*! @error condition priority. */
#define NT_LOG_LVL_ERR 2
/*! @critical condition priority. */
#define NT_LOG_LVL_CRIT 3
#endif
uint8_t min_loglvl = 1; // warn

static int cmd_qcc730_set_logger_lvl(const struct shell *ctx, size_t argc, char **argv)
{
    if (argc >= 2) {
        int err = 0;
        long rts_val = shell_strtol(argv[1], 10, &err);
        if (err) {
            shell_error(ctx, "Unable to parse input (err %d)", err);
            return err;
        }
        shell_print(ctx, "min_loglvl=%d=>%d for nt_logger", min_loglvl, rts_val);
        min_loglvl = (uint8_t)rts_val;
    }
    return 0;
}

static int cmd_qcc730_info(const struct shell *ctx, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_print(ctx, "min_loglvl=%d for nt_logger", min_loglvl);
    shell_print(ctx, "g32_dead_loop_1=%d for dead_loop_cond1(), generally used before sleep", g32_dead_loop_1);
    shell_print(ctx, "g32_dead_loop_2=%d for dead_loop_cond2(), generally used after sleep", g32_dead_loop_2);
    return 0;
}

static int cmd_qcc730_reboot(const struct shell *ctx, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_print(ctx, "Reboot...");
    nt_system_sw_reset();
    return 0;
}

static int cmd_qcc730_setdbg(const struct shell *ctx, size_t argc, char **argv)
{
    if (argc != 3) {
        shell_error(ctx, "parameters count not right (cnt %d), should be 3", argc);
        return -EINVAL;
    }

    int err = 0;
    uint32_t dbg_type = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse input dbg_type (err %d)", err);
        return err;
    }

    uint32_t dbg_value = shell_strtoul(argv[2], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse input dbg_value (err %d)", err);
        return err;
    }

    shell_print(ctx, "dbg_type=%d", dbg_type);
    shell_print(ctx, "dbg_value=%d", dbg_value);

    switch (dbg_type) {
    case 1:
        shell_print(ctx, "set g32_dead_loop_1 %d=>%d", g32_dead_loop_1, dbg_value);
        g32_dead_loop_1 = dbg_value;
        break;
    case 2:
        shell_print(ctx, "set g32_dead_loop_2 %d=>%d", g32_dead_loop_2, dbg_value);
        g32_dead_loop_2 = dbg_value;
        break;
    default:
        shell_warn(ctx, "dbg_type=%d not supported yet", dbg_type);
    }

    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_uart_cmds,
    SHELL_CMD_ARG(test1, NULL,
                  "test1\n"
                  "Usage: qcc730 test1\n",
                  cmd_qcc730_test1, 1, 0),
    SHELL_CMD_ARG(test2, NULL,
                  "test2\n"
                  "Usage: qcc730 test2\n",
                  cmd_qcc730_test2, 1, 0),
    SHELL_CMD_ARG(setloglvl, NULL,
                  "setloglvl\n"
                  "Usage: qcc730 setloglvl [n], n=0/1/2/3 for info/warn/err/crit for nt_logger\n",
                  cmd_qcc730_set_logger_lvl, 2, 0),
    SHELL_CMD_ARG(info, NULL,
                  "info\n"
                  "Usage: qcc730 info\n",
                  cmd_qcc730_info, 1, 0),
    SHELL_CMD_ARG(reboot, NULL,
                  "reboot\n"
                  "Usage: qcc730 reboot\n",
                  cmd_qcc730_reboot, 1, 0),
    SHELL_CMD_ARG(setdbg, NULL,
                  "setdbg\n"
                  "Usage: qcc730 setdbg [dbg_type:uint32_t] [dbg_value: uint32_t]\n",
                  cmd_qcc730_setdbg, 3, 0),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(qcc730, &sub_uart_cmds, "qcc730 test commands", NULL);
