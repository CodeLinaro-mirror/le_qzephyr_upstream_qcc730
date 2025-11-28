/**
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef QCOM_INCLUDE_NET_WIFI_MGMT_H_
#define QCOM_INCLUDE_NET_WIFI_MGMT_H_

#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/offloaded_netdev.h>
#include <zephyr/net/wifi_mgmt.h>

/** @brief Qcom Wi-Fi management commands */
enum qcom_net_request_wifi_cmd {
	/** Set TX Power for Wi-Fi networks */
	NET_REQUEST_WIFI_CMD_QCOM_SET_TX_POWER = 1,
	/** Get TX Power for Wi-Fi networks */
	NET_REQUEST_WIFI_CMD_QCOM_GET_TX_POWER
};

/** Request a Wi-Fi set tx power */
#define NET_REQUEST_WIFI_QCOM_SET_TX_POWER					\
	(_NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_SET_TX_POWER)

NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_TX_POWER);

/** Request a Wi-Fi get tx power */
#define NET_REQUEST_WIFI_QCOM_GET_TX_POWER					\
	(_NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_GET_TX_POWER)

NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_TX_POWER);

/** Set TX Power parameters */
struct qcom_wifi_set_tx_power_params{
	/** TX Power to be set */
    uint8_t txpower;
	/** Policy for setting TX Power */
    uint8_t policy;
};

/** Get TX Power parameters */
struct qcom_wifi_get_tx_power_params{
	/** Regulatory power */
    uint8_t reg_power;
	/** Conformance test limit power */
    uint8_t ctl_power;
	/** The targeted maximum TX power */
    uint16_t target_power;
	/** The power that is set to driver */
    uint16_t real_power;
};

/** Wi-Fi management API */
struct qcom_wifi_mgmt_ops {
	/** Set TX Power for Wi-Fi networks
	 *
	 * @param dev Pointer to the device structure for the driver instance.
	 * @param params Set TX Power parameters
	 *
	 * @return 0 if ok, < 0 if error
	 */
	int (*set_tx_power)(const struct device *dev,
		    struct qcom_wifi_set_tx_power_params *params);
	/** Get TX Power for Wi-Fi networks
	 *
	 * @param dev Pointer to the device structure for the driver instance.
	 * @param params Get TX Power parameters
	 *
	 * @return 0 if ok, < 0 if error
	 */
	int (*get_tx_power)(const struct device *dev,
		    struct qcom_wifi_get_tx_power_params *params);
};

#endif