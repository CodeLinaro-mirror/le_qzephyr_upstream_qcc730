/**
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef QCOM_INCLUDE_NET_WIFI_MGMT_H_
#define QCOM_INCLUDE_NET_WIFI_MGMT_H_

#include <zephyr/net/wifi_mgmt.h>
#include <stdint.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/offloaded_netdev.h>
#include "qapi_wlan_misc.h"

/** @brief Qcom Wi-Fi management commands */
enum qcom_net_request_wifi_cmd {
	NET_REQUEST_WIFI_CMD_QCOM_BASE = NET_REQUEST_WIFI_CMD_MAX,
	/** Set TX Power for Wi-Fi networks */
	NET_REQUEST_WIFI_CMD_QCOM_SET_TX_POWER,
	/** Get TX Power for Wi-Fi networks */
	NET_REQUEST_WIFI_CMD_QCOM_GET_TX_POWER,
	/** Unit test dispatch */
	NET_REQUEST_WIFI_CMD_QCOM_UNIT_TEST,
	/** Set RTS/CTS enable status */
	NET_REQUEST_WIFI_CMD_QCOM_SET_RTS_CTS,
	/** Get RTS/CTS enable status */
	NET_REQUEST_WIFI_CMD_QCOM_GET_RTS_CTS,
	/** Set RTS rate */
	NET_REQUEST_WIFI_CMD_QCOM_SET_RTS_RATE,
	/** Get RTS rate */
	NET_REQUEST_WIFI_CMD_QCOM_GET_RTS_RATE,
	/** Set EDCA (WMM) parameters */
	NET_REQUEST_WIFI_CMD_QCOM_SET_EDCA_PARAM_CFG,
	/** Get EDCA (WMM) parameters */
	NET_REQUEST_WIFI_CMD_QCOM_GET_EDCA_PARAM_CFG,
	/** Set PER upper threshold */
	NET_REQUEST_WIFI_CMD_QCOM_SET_THRESHOLD,
	/** Get PER upper threshold */
	NET_REQUEST_WIFI_CMD_QCOM_GET_THRESHOLD,
	/** Set BA window size parameters */
	NET_REQUEST_WIFI_CMD_QCOM_SET_BA_WIN_TIMING,
	/** Get BA window size parameters */
	NET_REQUEST_WIFI_CMD_QCOM_GET_BA_WIN_TIMING,
	/** Set PHY slot time */
	NET_REQUEST_WIFI_CMD_QCOM_SET_SLOT_TIME,
	/** Get PHY slot time */
	NET_REQUEST_WIFI_CMD_QCOM_GET_SLOT_TIME,
	/** Set STA BMISS threshold */
	NET_REQUEST_WIFI_CMD_QCOM_SET_BMISS_THRESHOLD,
	/** Get STA BMISS threshold */
	NET_REQUEST_WIFI_CMD_QCOM_GET_BMISS_THRESHOLD,
	/** Set PHY mode */
	NET_REQUEST_WIFI_CMD_QCOM_SET_PHY_MODE,
	/** Get PHY mode */
	NET_REQUEST_WIFI_CMD_QCOM_GET_PHY_MODE,
	/** Set aggregation TID masks */
	NET_REQUEST_WIFI_CMD_QCOM_SET_AGGREGATION,
	/** Set AMSDU RX enable/disable */
	NET_REQUEST_WIFI_CMD_QCOM_SET_AMSDU_RX,
	/** Set data rate */
	NET_REQUEST_WIFI_CMD_QCOM_SET_RATE,
	/** Get data rate */
	NET_REQUEST_WIFI_CMD_QCOM_GET_RATE,
	/** Get power mode */
	NET_REQUEST_WIFI_CMD_QCOM_GET_POWER_MODE,
	/** Get MAC address */
	NET_REQUEST_WIFI_CMD_QCOM_GET_MAC_ADDRESS,
	/** Get concurrency mode */
	NET_REQUEST_WIFI_CMD_QCOM_GET_CONCURRENCY_MODE,
	/** Get operation mode */
	NET_REQUEST_WIFI_CMD_QCOM_GET_OPERATION_MODE,
	/** Get boot reason */
	NET_REQUEST_WIFI_CMD_QCOM_GET_BOOT_REASON,
	NET_REQUEST_WIFI_CMD_QCOM_MAX,
};

/** Request a Wi-Fi set tx power */
#define NET_REQUEST_WIFI_QCOM_SET_TX_POWER					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_SET_TX_POWER)

NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_TX_POWER);

/** Request a Wi-Fi get tx power */
#define NET_REQUEST_WIFI_QCOM_GET_TX_POWER					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_GET_TX_POWER)

NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_TX_POWER);

#define NET_REQUEST_WIFI_QCOM_UNIT_TEST					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_UNIT_TEST)

NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_UNIT_TEST);

/* Set RTS/CTS enable */
#define NET_REQUEST_WIFI_QCOM_SET_RTS_CTS					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_SET_RTS_CTS)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_RTS_CTS);

/* Get RTS/CTS enable */
#define NET_REQUEST_WIFI_QCOM_GET_RTS_CTS					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_GET_RTS_CTS)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_RTS_CTS);

/* Set RTS rate (2.4 GHz) */
#define NET_REQUEST_WIFI_QCOM_SET_RTS_RATE					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_SET_RTS_RATE)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_RTS_RATE);

/* Get RTS rate (2.4 GHz) */
#define NET_REQUEST_WIFI_QCOM_GET_RTS_RATE					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_GET_RTS_RATE)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_RTS_RATE);

/* Set EDCA (WMM) parameters */
#define NET_REQUEST_WIFI_QCOM_SET_EDCA_PARAM_CFG				\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_SET_EDCA_PARAM_CFG)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_EDCA_PARAM_CFG);

/* Get EDCA (WMM) parameters */
#define NET_REQUEST_WIFI_QCOM_GET_EDCA_PARAM_CFG				\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_GET_EDCA_PARAM_CFG)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_EDCA_PARAM_CFG);

/* Set PER upper threshold */
#define NET_REQUEST_WIFI_QCOM_SET_THRESHOLD					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_SET_THRESHOLD)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_THRESHOLD);

/* Get PER upper threshold */
#define NET_REQUEST_WIFI_QCOM_GET_THRESHOLD					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_GET_THRESHOLD)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_THRESHOLD);

/* Set BA window size parameters */
#define NET_REQUEST_WIFI_QCOM_SET_BA_WIN_TIMING					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_SET_BA_WIN_TIMING)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_BA_WIN_TIMING);

/* Get BA window size parameters */
#define NET_REQUEST_WIFI_QCOM_GET_BA_WIN_TIMING					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_GET_BA_WIN_TIMING)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_BA_WIN_TIMING);

/* Set PHY slot time */
#define NET_REQUEST_WIFI_QCOM_SET_SLOT_TIME					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_SET_SLOT_TIME)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_SLOT_TIME);

/* Get PHY slot time */
#define NET_REQUEST_WIFI_QCOM_GET_SLOT_TIME					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_GET_SLOT_TIME)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_SLOT_TIME);

/* Set STA BMISS threshold */
#define NET_REQUEST_WIFI_QCOM_SET_BMISS_THRESHOLD				\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_SET_BMISS_THRESHOLD)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_BMISS_THRESHOLD);

/* Get STA BMISS threshold */
#define NET_REQUEST_WIFI_QCOM_GET_BMISS_THRESHOLD				\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_GET_BMISS_THRESHOLD)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_BMISS_THRESHOLD);

/* Set PHY mode */
#define NET_REQUEST_WIFI_QCOM_SET_PHY_MODE					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_SET_PHY_MODE)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_PHY_MODE);

/* Get PHY mode */
#define NET_REQUEST_WIFI_QCOM_GET_PHY_MODE					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_GET_PHY_MODE)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_PHY_MODE);

/* Set aggregation TID masks */
#define NET_REQUEST_WIFI_QCOM_SET_AGGREGATION					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_SET_AGGREGATION)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_AGGREGATION);

/* Set AMSDU RX enable */
#define NET_REQUEST_WIFI_QCOM_SET_AMSDU_RX					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_SET_AMSDU_RX)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_AMSDU_RX);

/* Set data rate */
#define NET_REQUEST_WIFI_QCOM_SET_RATE					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_SET_RATE)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_RATE);

/* Get data rate */
#define NET_REQUEST_WIFI_QCOM_GET_RATE					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_GET_RATE)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_RATE);

/* Get boot reason */
#define NET_REQUEST_WIFI_QCOM_GET_BOOT_REASON					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_GET_BOOT_REASON)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_BOOT_REASON);

/* Get power mode */
#define NET_REQUEST_WIFI_QCOM_GET_POWER_MODE					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_GET_POWER_MODE)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_POWER_MODE);

/* Get MAC address */
#define NET_REQUEST_WIFI_QCOM_GET_MAC_ADDRESS					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_GET_MAC_ADDRESS)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_MAC_ADDRESS);

/* Get concurrency mode */
#define NET_REQUEST_WIFI_QCOM_GET_CONCURRENCY_MODE					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_GET_CONCURRENCY_MODE)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_CONCURRENCY_MODE);

/* Get operation mode */
#define NET_REQUEST_WIFI_QCOM_GET_OPERATION_MODE					\
	(NET_WIFI_BASE | NET_REQUEST_WIFI_CMD_QCOM_GET_OPERATION_MODE)
NET_MGMT_DEFINE_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_OPERATION_MODE);

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

/** Get RTS/CTS enable parameters */
struct qcom_wifi_get_rts_cts_params {
	/** RTS/CTS control flag: 1 enable, 0 disable */
	uint32_t enable;
};

/** Get RTS rate parameters (2.4 GHz) */
struct qcom_wifi_get_rts_rate_params {
	/** RTS rate selector (implementation-defined mapping) */
	uint32_t rts_rate;
};

/** Get EDCA (WMM) parameters */
struct qcom_wifi_get_edca_param_cfg_params {
	/** Queue id: 0..7 or 0xFF for all queues (implementation-defined) */
	uint8_t qid;
	/** Arbitration Inter-Frame Space Number */
	uint8_t aifsn;
	/** Minimum contention window exponent */
	uint16_t cw_min;
	/** Maximum contention window exponent */
	uint16_t cw_max;
	/** Transmit opportunity limit */
	uint16_t txop_limit;
};

/** Get PER upper threshold parameters */
struct qcom_wifi_get_threshold_params {
	/** Upper PER threshold value */
	uint32_t threshold;
};

/** Get BA window size parameters */
struct qcom_wifi_get_ba_win_timing_params {
	/** ACK timeout in microseconds */
	uint16_t ack_timeout;
	/** Delay value in SM clock cycles */
	uint16_t delay;
};

/** Get PHY slot time parameters */
struct qcom_wifi_get_slot_time_params {
	/** Slot time in microseconds */
	uint32_t slot_time;
};

/** Set RTS/CTS enable parameters */
struct qcom_wifi_set_rts_cts_params {
	/** RTS/CTS control flag: 1 enable, 0 disable */
	uint32_t enable;
};

/** Set RTS rate parameters (2.4 GHz) */
struct qcom_wifi_set_rts_rate_params {
	/** RTS rate selector (implementation-defined mapping) */
	uint32_t rts_rate;
};

/** Set EDCA (WMM) parameters */
struct qcom_wifi_set_edca_param_cfg_params {
	/** Queue id: 0..7 or 0xFF for all queues (implementation-defined) */
	uint8_t qid;
	/** Arbitration Inter-Frame Space Number */
	uint8_t aifsn;
	/** Minimum contention window exponent */
	uint16_t cw_min;
	/** Maximum contention window exponent */
	uint16_t cw_max;
	/** Transmit opportunity limit */
	uint16_t txop_limit;
};

/** Set PER upper threshold parameters */
struct qcom_wifi_set_threshold_params {
	/** Upper PER threshold value */
	uint32_t threshold;
};

/** Set BA window size parameters */
struct qcom_wifi_set_ba_win_timing_params {
	/** ACK timeout in microseconds */
	uint16_t ack_timeout;
	/** Delay value in SM clock cycles */
	uint16_t delay;
};

/** Set PHY slot time parameters */
struct qcom_wifi_set_slot_time_params {
	/** Slot time in microseconds */
	uint32_t slot_time;
};

/** Set STA BMISS threshold parameters */
struct qcom_wifi_set_bmiss_threshold_params {
	/** BMISS threshold (missed beacons before disconnect/trigger) */
	uint32_t threshold;
};

/** Get STA BMISS threshold parameters */
struct qcom_wifi_get_bmiss_threshold_params {
	/** BMISS threshold (missed beacons before disconnect/trigger) */
	uint32_t threshold;
};

/** Set PHY mode parameters */
struct qcom_wifi_set_phy_mode_params {
	/** PHY mode value (qapi_WLAN_Phy_Mode_e) */
	uint32_t phy_mode;
};

/** Get PHY mode parameters */
struct qcom_wifi_get_phy_mode_params {
	/** PHY mode value (qapi_WLAN_Phy_Mode_e) */
	uint32_t phy_mode;
};

/** Get power mode parameters */
struct qcom_wifi_get_power_mode_params {
	/** Power mode bitfield (0: Max Perf, non-zero indicates Power Save flags) */
	uint8_t power_mode;
};

/** Get boot reason parameters */
struct qcom_wifi_get_boot_reason_params {
	/** Boot reason bitfield */
	uint32_t boot_reason;
};

/** Get MAC address parameters */
struct qcom_wifi_get_mac_address_params {
	/** MAC address (__QAPI_WLAN_MAC_LEN bytes) */
	uint8_t mac[__QAPI_WLAN_MAC_LEN];
};

/** Get concurrency mode parameters */
struct qcom_wifi_get_concurrency_mode_params {
	/** Concurrency mode (qapi_WLAN_DEV_Mode_e) */
	uint32_t conc_mode;
};

/** Get operation mode parameters */
struct qcom_wifi_get_operation_mode_params {
	/** Operation mode (qapi_WLAN_DEV_Mode_e) */
	uint32_t opmode;
};

/** Set aggregation TID masks parameters */
struct qcom_wifi_set_aggregation_params {
	/** TX aggregation TID bitmask (bit i enables aggregation for TID i) */
	uint8_t tx_tid_mask;
	/** RX aggregation TID bitmask (bit i enables aggregation for TID i) */
	uint8_t rx_tid_mask;
};

/** Set AMSDU RX enable/disable parameters */
struct qcom_wifi_set_amsdu_rx_params {
	/** AMSDU RX control flag: 1 enable, 0 disable */
	uint8_t enable;
};

/** Set/Get data rate parameters (wrapper around QAPI struct) */
struct qcom_wifi_set_rate_params {
	/** 1: auto rate, 0: manual */
	uint8_t ra_ON;
	/** Station ID */
	uint8_t rate_staid;
	/** Primary rate */
	uint8_t rate_p_rate;
	/** Secondary rate */
	uint8_t rate_s_rate;
	/** Tertiary rate */
	uint8_t rate_t_rate;
};

/** Unit test dispatch params */
struct qcom_wifi_unit_test_params {
	uint8_t vdev_id;
	uint8_t module_id;
	uint16_t num_args;
	uint32_t args[16];
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
	/**
	 * @brief Perform a unit test.
	 *
	 * @param dev Pointer to the device structure for the driver instance.
	 * @param params Unit test parameters.
	 *
	 * @return 0 if ok, < 0 if error.
	 */
	int (*unit_test)(const struct device *dev,
			struct qcom_wifi_unit_test_params *params);

	/**
	 * @brief Enable or disable RTS/CTS protection on the active WLAN device.
	 *
	 * Controls RTS/CTS protection via qapi_WLAN_Set_Param for the currently active
	 * WLAN interface. When enabled, RTS/CTS handshaking can reduce collisions in
	 * congested or hidden-node scenarios by requiring a request-to-send and
	 * clear-to-send exchange before data transmission.
	 *
	 * @param rts_cts RTS/CTS control flag:
	 *        - 1: enable RTS/CTS protection
	 *        - 0: disable RTS/CTS protection
	 *
	 * @return 0 on success; -1 on failure.
	 */
	int (*set_rts_cts)(const struct device *dev,
			struct qcom_wifi_set_rts_cts_params *params);

	/**
	 * @brief Set the RTS control frame transmit rate on the active WLAN device (2.4 GHz).
	 *
	 * Configures the transmit rate used for RTS/CTS control frames via qapi_WLAN_Set_Param
	 * on the currently active interface. Adjusting the RTS rate can influence airtime and
	 * robustness of the RTS/CTS protection mechanism, particularly on 2.4 GHz links.
	 *
	 * Preconditions:
	 * - Operates on the active device returned by get_active_device().
	 * - Intended for 2.4 GHz operation (uses __QAPI_WLAN_PARAM_GROUP_WIRELESS_RTS_RATE_2G).
	 *   The accompanying comment suggests using this after a 2G connection.
	 *
	 * @param rate RTS rate selector (indexed mapping):
	 *        - 0: 1 Mbps (802.11b long)
	 *        - 1: 6 Mbps (OFDM)
	 *        - 2: 12 Mbps (OFDM)
	 *   Note: Valid values and their mapping are defined by the underlying firmware/QAPI
	 *   and may be limited to the options shown above.
	 *
	 * @return 0 on success; -1 on failure.
	 */
	int (*set_rts_rate)(const struct device *dev,
			struct qcom_wifi_set_rts_rate_params *params);

	/**
	 * @brief Configure EDCA (WMM) parameters on the active WLAN device.
	 *
	 * Programs the per-queue Enhanced Distributed Channel Access (EDCA) parameters
	 * (AIFSN, CWmin, CWmax, and TXOP limit) via qapi_WLAN_Set_Param for the queue
	 * identified by qid, or for all queues when qid == 0xFF.
	 *
	 * @param qid        Access category/queue identifier:
	 *                   - 0..7: apply to the specified hardware/software queue
	 *                   - 0xFF: apply to all queues
	 * @param aifsn      Arbitration Inter-Frame Space Number. Lower values give higher priority.
	 * @param cw_min     Minimum contention window exponent e_min. Effective CWmin = 2^e_min - 1.
	 * @param cw_max     Maximum contention window exponent e_max. Effective CWmax = 2^e_max - 1.
	 * @param txop_limit Transmit opportunity limit for the queue; duration as defined by the
	 *                   firmware/QAPI (commonly in 32 µs units). A value of 0 typically disables
	 *                   bursting for the queue.
	 *
	 * @return 0 on success; -1 on failure.
	 *
	 * Notes:
	 * - Operates on the active device (get_active_device()).
	 * - Ensure values adhere to firmware/regulatory bounds; invalid values will be rejected
	 *   by qapi_WLAN_Set_Param.
	 */
	int (*set_edca_param_cfg)(const struct device *dev,
			struct qcom_wifi_set_edca_param_cfg_params *params);

	/**
	 * @brief Set the upper Packet Error Rate (PER) threshold on the active WLAN device.
	 *
	 * Configures the PER upper threshold via qapi_WLAN_Set_Param for the currently
	 * active interface. This threshold can be used by firmware to trigger internal
	 * algorithms (e.g., rate control or diagnostics) when the measured PER exceeds
	 * the configured limit.
	 *
	 * @param value Upper PER threshold value. The valid range is enforced by the
	 *              firmware; values must be less than 100 (implementation-specific
	 *              units; commonly interpreted as percentage 0..99).
	 *
	 * @return 0 on success; -1 on failure.
	 *
	 * Notes:
	 * - Operates on the active device (get_active_device()).
	 * - The function fails if the driver/firmware rejects the provided threshold.
	 */
	int (*set_threshold)(const struct device *dev,
			struct qcom_wifi_set_threshold_params *params);

	/**
	 * @brief Configure Block Ack (BA) window timing parameters on the active WLAN device.
	 *
	 * Programs the BA window parameters (ACK timeout and delay) via qapi_WLAN_Set_Param
	 * for the currently active interface. These timing values influence the Block Ack
	 * exchange behavior and can affect both reliability and throughput.
	 *
	 * @param ack_time   ACK timeout in microseconds. Must be less than 4096 µs.
	 * @param delay_time Delay value in SM clock cycles (firmware-specific units).
	 *                   Typically constrained to less than 64 SM cycles.
	 *
	 * @return 0 on success; -1 on failure.
	 *
	 * Notes:
	 * - Operates on the active device returned by get_active_device().
	 * - The firmware enforces valid ranges; invalid values are rejected by qapi_WLAN_Set_Param.
	 */
	int (*set_ba_win_timing)(const struct device *dev,
			struct qcom_wifi_set_ba_win_timing_params *params);

	/**
	 * @brief Set the PHY slot time on the active WLAN device.
	 *
	 * Configures the slot time used by the MAC backoff algorithm via qapi_WLAN_Set_Param
	 * for the currently active interface. Typical values are 9 µs (short slot) and 20 µs
	 * (long slot), depending on PHY mode and regulatory/compatibility constraints.
	 *
	 * @param time Slot time in microseconds (commonly 9 or 20).
	 *
	 * @return 0 on success; -1 on failure.
	 *
	 * Notes:
	 * - Operates on the active device returned by get_active_device().
	 * - The firmware enforces valid slot times; invalid values are rejected by qapi_WLAN_Set_Param.
	 */
	int (*set_slot_time)(const struct device *dev,
			struct qcom_wifi_set_slot_time_params *params);

	/**
	 * @brief Set STA BMISS threshold (missed beacons before triggering action).
	 *
	 * Configures the BMISS threshold via qapi_WLAN_Set_Param using
	 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_STA_BMISS_CONFIG.
	 *
	 * @param dev Pointer to the driver device instance.
	 * @param params Input structure with params->threshold (firmware-defined range).
	 *
	 * @return 0 if ok, < 0 if error.
	 */
	int (*set_bmiss_threshold)(const struct device *dev,
			struct qcom_wifi_set_bmiss_threshold_params *params);

	/**
	 * @brief Get RTS/CTS protection enable status.
	 *
	 * Retrieves the current RTS/CTS protection flag from the active WLAN device
	 * via qapi_WLAN_Get_Param using __QAPI_WLAN_PARAM_GROUP_WIRELESS_RTS.
	 *
	 * @param dev Pointer to the device structure for the driver instance.
	 * @param params Output structure; on success, params->enable is set to:
	 *        - 1: RTS/CTS enabled
	 *        - 0: RTS/CTS disabled
	 *
	 * @return 0 if ok, < 0 if error.
	 */
	int (*get_rts_cts)(const struct device *dev,
			struct qcom_wifi_get_rts_cts_params *params);

	/**
	 * @brief Get the RTS control frame transmit rate (2.4 GHz).
	 *
	 * Reads the RTS rate selector used on 2.4 GHz via qapi_WLAN_Get_Param
	 * with __QAPI_WLAN_PARAM_GROUP_WIRELESS_RTS_RATE_2G.
	 *
	 * @param dev Pointer to the driver device instance.
	 * @param params Output structure; on success, params->rts_rate contains the
	 *        firmware-defined rate index (e.g. 0: 1 Mbps, 1: 6 Mbps, 2: 12 Mbps).
	 *
	 * @return 0 if ok, < 0 if error.
	 */
	int (*get_rts_rate)(const struct device *dev,
			struct qcom_wifi_get_rts_rate_params *params);

	/**
	 * @brief Get EDCA (WMM) parameters for a queue or all queues.
	 *
	 * Retrieves the EDCA parameters (AIFSN, CWmin, CWmax, TXOP limit) via
	 * qapi_WLAN_Get_Param with __QAPI_WLAN_PARAM_GROUP_WIRELESS_EDCA_PARAM.
	 *
	 * @param dev Pointer to the driver device instance.
	 * @param params Input/Output structure:
	 *        - Input: params->qid selects the queue (0..7) or 0xFF for all queues,
	 *                 when supported by firmware.
	 *        - Output: params->qid, params->aifsn, params->cw_min, params->cw_max,
	 *                  params->txop_limit are filled with current configuration.
	 *
	 * @return 0 if ok, < 0 if error.
	 *
	 * Notes:
	 * - Queue selector semantics depend on firmware support.
	 */
	int (*get_edca_param_cfg)(const struct device *dev,
			struct qcom_wifi_get_edca_param_cfg_params *params);

	/**
	 * @brief Get the upper Packet Error Rate (PER) threshold.
	 *
	 * Retrieves the configured PER upper threshold via
	 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_PER_UPPER_THRESHOLD.
	 *
	 * @param dev Pointer to the driver device instance.
	 * @param params Output structure; on success, params->threshold holds the
	 *        current PER threshold (firmware-defined units; commonly 0..99).
	 *
	 * @return 0 if ok, < 0 if error.
	 */
	int (*get_threshold)(const struct device *dev,
			struct qcom_wifi_get_threshold_params *params);

	/**
	 * @brief Get Block Ack (BA) window timing parameters.
	 *
	 * Reads the BA window configuration (ACK timeout and delay) via
	 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_BA_WINDOW.
	 *
	 * @param dev Pointer to the driver device instance.
	 * @param params Output structure; on success:
	 *        - params->ack_timeout: ACK timeout in microseconds
	 *        - params->delay: delay value in SM clock cycles
	 *
	 * @return 0 if ok, < 0 if error.
	 */
	int (*get_ba_win_timing)(const struct device *dev,
			struct qcom_wifi_get_ba_win_timing_params *params);

	/**
	 * @brief Get the PHY slot time in microseconds.
	 *
	 * Retrieves the MAC slot time used for backoff via
	 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_SLOT_TIME.
	 *
	 * @param dev Pointer to the driver device instance.
	 * @param params Output structure; on success, params->slot_time is set
	 *        to the current slot time (e.g., 9 or 20 µs).
	 *
	 * @return 0 if ok, < 0 if error.
	 */
	int (*get_slot_time)(const struct device *dev,
			struct qcom_wifi_get_slot_time_params *params);

	/**
	 * @brief Get STA BMISS threshold.
	 *
	 * Retrieves the BMISS threshold via qapi_WLAN_Get_Param using
	 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_STA_BMISS_CONFIG.
	 *
	 * @param dev Pointer to the driver device instance.
	 * @param params Output structure; on success params->threshold is set.
	 *
	 * @return 0 if ok, < 0 if error.
	 */
	int (*get_bmiss_threshold)(const struct device *dev,
			struct qcom_wifi_get_bmiss_threshold_params *params);

	/**
	 * @brief Set PHY mode on the active WLAN device.
	 *
	 * Configures PHY mode via qapi_WLAN_Set_Param using
	 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_PHY_MODE.
	 *
	 * @param dev Pointer to the driver device instance.
	 * @param params Input structure with params->phy_mode (qapi_WLAN_Phy_Mode_e).
	 *
	 * @return 0 if ok, < 0 if error.
	 */
	int (*set_phy_mode)(const struct device *dev,
			struct qcom_wifi_set_phy_mode_params *params);

	/**
	 * @brief Get PHY mode on the active WLAN device.
	 *
	 * Retrieves PHY mode via qapi_WLAN_Get_Param using
	 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_PHY_MODE.
	 *
	 * @param dev Pointer to the driver device instance.
	 * @param params Output structure with params->phy_mode (qapi_WLAN_Phy_Mode_e).
	 *
	 * @return 0 if ok, < 0 if error.
	 */
	int (*get_phy_mode)(const struct device *dev,
			struct qcom_wifi_get_phy_mode_params *params);
	/**
	 * @brief Get Wi-Fi power mode of the active device.
	 *
	 * Retrieves the current power performance/save mode via qapi_WLAN_Get_Param
	 * using __QAPI_WLAN_PARAM_GROUP_WIRELESS_POWER_MODE_PARAMS on the active device.
	 *
	 * On success, params->power_mode contains:
	 *  - 0: Max Perf (no power saving)
	 *  - bit0 (1): BMPS enabled (Beacon Mode Power Save)
	 *  - bit1 (2): IMPS enabled (Idle Mode Power Save)
	 *  - bit2 (4): WUR enabled (Wake-Up Radio)
	 *  - bit3 (8): WNM enabled (Wireless Network Management power features)
	 *
	 * @param dev    Pointer to the driver device instance.
	 * @param params Output structure of type qcom_wifi_get_power_mode_params;
	 *               on success, params->power_mode is filled with the bitfield above.
	 *
	 * @return 0 if ok, < 0 if error.
	 */
	int (*get_power_mode)(const struct device *dev,
			struct qcom_wifi_get_power_mode_params *params);

	/**
	 * @brief Get system boot reason bitfield.
	 *
	 * Retrieves the raw boot-reason bitfield from the platform via the driver.
	 * The underlying driver typically calls qapi_Core_Obtain_Boot_Reason() to
	 * obtain this value.
	 *
	 * On success, params->boot_reason contains a 32-bit bitfield whose
	 * interpretation is platform-specific (e.g., cold/warm boot, wake from DTIM
	 * sleep, wake from deep sleep). Decoding can be performed at higher layers
	 * using platform-defined mask macros.
	 *
	 * @param dev    Pointer to the driver device instance.
	 * @param params Output structure of type qcom_wifi_get_boot_reason_params; on
	 *               success, params->boot_reason is filled with the bitfield.
	 *
	 * @return 0 if ok; negative error code if failure.
	 */
	int (*get_boot_reason)(const struct device *dev,
			struct qcom_wifi_get_boot_reason_params *params);

	/**
	 * @brief Get MAC address of the active WLAN device.
	 *
	 * Retrieves the device MAC address via qapi_WLAN_Get_Param using
	 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_MAC_ADDRESS on the active device.
	 *
	 * On success, params->mac is filled with ETH_ALEN (6) bytes of the MAC address.
	 *
	 * @param dev    Pointer to the driver device instance.
	 * @param params Output structure of type qcom_wifi_get_mac_address_params;
	 *               on success, params->mac[] contains the device MAC.
	 *
	 * @return 0 if ok, < 0 if error.
	 */
	int (*get_mac_address)(const struct device *dev,
			struct qcom_wifi_get_mac_address_params *params);
	/**
	 * @brief Get WLAN concurrency mode of the active device.
	 *
	 * Retrieves concurrency mode via qapi_WLAN_Get_Param using
	 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_CONCURRENCY_MODE.
	 *
	 * On success, params->conc_mode holds the concurrency mode enum
	 * (qapi_WLAN_DEV_Mode_e), e.g. DEV_MODE_AP_STA_E for AP+STA concurrency,
	 * or a single-mode value when concurrency is disabled.
	 *
	 * @param dev    Pointer to the driver device instance.
	 * @param params Output structure of type qcom_wifi_get_concurrency_mode_params;
	 *               on success, params->conc_mode is set to the current mode.
	 *
	 * @return 0 if ok, < 0 if error.
	 */
	int (*get_concurrency_mode)(const struct device *dev,
			struct qcom_wifi_get_concurrency_mode_params *params);
	/**
	 * @brief Get WLAN operation mode of the active device.
	 *
	 * Retrieves operation mode via qapi_WLAN_Get_Param using
	 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_OPERATION_MODE.
	 *
	 * On success, params->opmode holds the operation mode enum
	 * (qapi_WLAN_DEV_Mode_e), typically DEV_MODE_STATION_E for STA
	 * or DEV_MODE_AP_E for SoftAP.
	 *
	 * @param dev    Pointer to the driver device instance.
	 * @param params Output structure of type qcom_wifi_get_operation_mode_params;
	 *               on success, params->opmode is set to the current operation mode.
	 *
	 * @return 0 if ok, < 0 if error.
	 */
	int (*get_operation_mode)(const struct device *dev,
			struct qcom_wifi_get_operation_mode_params *params);

	/**
	 * @brief Configure TX/RX aggregation TID bitmasks on the active WLAN device.
	 *
	 * Programs aggregation enable masks via qapi_WLAN_Set_Param using
	 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_ALLOW_TX_RX_AGGR_SET_TID.
	 *
	 * @param dev Pointer to the driver device instance.
	 * @param params Input structure:
	 *        - params->tx_tid_mask: 8-bit bitmask; bit i (0..7) enables TX aggregation for TID i.
	 *        - params->rx_tid_mask: 8-bit bitmask; bit i (0..7) enables RX aggregation for TID i.
	 *
	 * @return 0 if ok, < 0 if error.
	 *
	 * Notes:
	 * - Operates on the currently active device returned by get_qcom_wifi_api().
	 * - Each mask is limited to 0..0xFF; invalid values are rejected by the shell prior to dispatch.
	 */
	int (*set_aggregation)(const struct device *dev,
			struct qcom_wifi_set_aggregation_params *params);
	/**
	 * @brief Enable or disable AMSDU RX on the active WLAN device.
	 *
	 * Configures AMSDU RX via qapi_WLAN_Set_Param using
	 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_AMSDU_RX.
	 *
	 * @param dev Pointer to the driver device instance.
	 * @param params Input structure with params->enable (1: enable, 0: disable).
	 *
	 * @return 0 if ok, < 0 if error.
	 */
	int (*set_amsdu_rx)(const struct device *dev,
			struct qcom_wifi_set_amsdu_rx_params *params);

	/**
	 * @brief Set data rate configuration (auto or manual rates).
	 *
	 * Calls qapi_WLAN_Set_Rate with the provided parameters.
	 *
	 * @param dev Pointer to the driver device instance.
	 * @param params Pointer to qapi_WLAN_Set_Rate_Params_t containing:
	 *        - ra_ON: 1 for auto rate, 0 for manual
	 *        - rate_staid: station ID
	 *        - rate_p_rate, rate_s_rate, rate_t_rate: primary/secondary/tertiary rates
	 * @return 0 if ok, < 0 if error.
	 */
	int (*set_rate)(const struct device *dev,
			struct qcom_wifi_set_rate_params *params);

	/**
	 * @brief Get data rate configuration for a station.
	 *
	 * Calls qapi_WLAN_Get_Rate to retrieve current rates.
	 *
	 * @param dev Pointer to the driver device instance.
	 * @param params Pointer to qapi_WLAN_Set_Rate_Params_t; on success,
	 *        rate_p_rate, rate_s_rate, rate_t_rate are filled.
	 * @return 0 if ok, < 0 if error.
	 */
	int (*get_rate)(const struct device *dev,
			struct qcom_wifi_set_rate_params *params);
};

#endif
