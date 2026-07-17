/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */


#ifndef QCOM_WPS_GLUE_H
#define QCOM_WPS_GLUE_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WPS scan result — passed internally via qwifi_wps_scan_event */
struct qcom_wps_scan_result {
    uint8_t  bssid[6];
    uint8_t  ssid[33];
    uint8_t  ssid_len;
    uint16_t channel;
    int8_t   rssi;
};

/**
 * qcom_wps_start_pbc - Top-level entry point for WPS PBC provisioning.
 *
 * This is the single function to call from a WPS PBC command handler.
 * It drives the full PBC flow:
 *   1. qcom_wps_scan()    — find PBC-active AP
 *   2. qcom_wps_connect() — associate (open) to the target AP
 *   3. EAP-WSC M1-M8 exchange (driven by wps_eap_rx / hostap)
 *   4. wps_context.cred_cb — credentials passed to qapi_WLAN_Connect()
 *
 * @dev           Zephyr device pointer
 * @bssid         Target AP BSSID filter, or NULL for any PBC-active AP.
 * @channels      Array of 802.11 channel numbers to restrict scanning to,
 *                or NULL for a full-channel scan.
 * @channel_count Number of entries in @channels; 0 if @channels is NULL.
 * @return  0 if the flow was started, negative errno on error.
 *          The result is asynchronous — wps_cred_cb fires on success,
 *          or qcom_wps_cancel() is called internally on failure.
 */
int qcom_wps_start_pbc(const struct device *dev,
                        const uint8_t  *bssid,
                        const uint16_t *channels,
                        uint8_t         channel_count);

/**
 * qcom_wps_connect_in_progress - Check if a WPS connect is pending assoc.
 *
 * Called from station_connect_event() to decide whether the assoc event
 * should be routed to qcom_wps_assoc_event().
 * True when PBC session is active and scan has completed (connect phase).
 */
bool qcom_wps_connect_in_progress(void);

/**
 * qcom_wps_connect - Associate to the WPS target AP (open mode).
 *
 * Called after scan finds the target AP.  Associates without credentials so
 * EAP-WSC can run.  Registers wps_eap_rx hook for EAP frame routing.
 * On completion, wps_context.cred_cb (wps_cred_cb) delivers credentials
 * directly to qapi_WLAN_Connect().
 *
 * @dev     Zephyr device pointer
 * @result  Target AP from scan
 * @return  0 on success, negative errno on error
 */
int qcom_wps_connect(const struct device *dev,
                     const struct qcom_wps_scan_result *result);

/**
 * qcom_wps_cancel - Cancel an in-progress WPS operation.
 *
 * Stops scan or disconnects, unregisters EAP hook, frees hostap wps_data.
 *
 * @dev  Zephyr device pointer
 */
void qcom_wps_cancel(const struct device *dev);

/**
 * qcom_wps_init - Initialise the WPS context for this interface.
 *
 * Allocates and fills struct wps_context with device info, config_methods,
 * auth/encr types and hostap callbacks (cred_cb, event_cb).
 * Must be called once when the Wi-Fi interface is enabled, before any
 * qcom_wps_start_pbc() call.
 * Mirrors wpas_wps_init() in wpa_supplicant.
 *
 * @dev  Zephyr device pointer
 * @return 0 on success, negative errno on error
 */
int qcom_wps_init(const struct device *dev);

/**
 * qcom_wps_deinit - Free the WPS context for this interface.
 *
 * Cancels any in-progress WPS session, then frees the wps_context.
 * Reserved for future use when a WiFi-disable path is added to the driver.
 * Currently the driver has no disable path so this is not called.
 *
 * @dev  Zephyr device pointer
 */
void qcom_wps_deinit(const struct device *dev);

/**
 * qcom_wps_assoc_event - Notify WPS glue of firmware 802.11 assoc completion.
 *
 * Called from station_connect_event() in qcom_wifi_drv.c when security type
 * is WPS and reason_code == RECEIVED_ASSOC_RESP.  Registers wps_eap_rx hook
 * and initialises hostap wps_data to start M1 generation.
 *
 * @dev      Zephyr device pointer
 * @bssid    AP BSSID (6 bytes)
 * @success  true if firmware reported successful assoc
 * @device_id QAPI device ID
 */
void qcom_wps_assoc_event(const struct device *dev, const uint8_t *bssid,
                           bool success, uint8_t device_id);

/**
 * qwifi_wps_sync_connect_params - Sync cfg_connect with WPS credentials.
 *
 * Updates dev_data->cfg_connect so that "wifi status" and net_mgmt events
 * reflect the correct SSID, security type and PSK after PSK reconnect.
 * Must be called before qapi_WLAN_Commit() in the WPS PSK reconnect path.
 *
 * @dev       Zephyr device pointer
 * @ssid      SSID bytes
 * @ssid_len  SSID length
 * @security  Zephyr security type (WIFI_SECURITY_TYPE_PSK etc.)
 * @psk       PSK bytes, or NULL for open
 * @psk_len   PSK length
 */
void qwifi_wps_sync_connect_params(const struct device *dev,
                                    const uint8_t *ssid, uint8_t ssid_len,
                                    enum wifi_security_type security,
                                    const uint8_t *psk, uint8_t psk_len);

/**
 * qwifi_wps_scan_event - WPS scan event callback, called from
 * qwifi_drv_event_handler() for WPS AP and scan-complete events.
 * Defined in qcom_wps_glue.c.
 */
void qwifi_wps_scan_event(uint32_t event_id, void *payload,
                           uint32_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* QCOM_WPS_GLUE_H */
