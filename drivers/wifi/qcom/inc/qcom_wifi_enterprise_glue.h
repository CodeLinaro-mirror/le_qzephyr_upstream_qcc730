/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef QCOM_WIFI_ENTERPRISE_GLUE_H
#define QCOM_WIFI_ENTERPRISE_GLUE_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/net/net_if.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zep_wpa_supp_dev_ops;
extern const struct zep_wpa_supp_dev_ops qcom_wifi_ent_drv_ops;

/**
 * qcom_ent_assoc_event - notify enterprise glue of firmware 802.11 assoc.
 *
 * Called from station_connect_event() when CONFIG_WIFI_QCOM_ENTERPRISE and
 * the security type is EAP.  Posts EVENT_ASSOC to wpa_supplicant so the
 * EAP state machine starts immediately after firmware assoc.
 *
 * @dev       Zephyr device pointer
 * @bssid     AP BSSID reported by firmware (6 bytes)
 * @success   true if firmware reported successful association
 * @device_id QAPI device ID (dev_data->active_device) — stored so that
 *            qcom_supp_set_key(KEY_FLAG_PMK) can call qapi_WLAN_Set_Param
 *            without needing access to qwifi_drv_dev_data_t.
 */
void qcom_ent_assoc_event(const struct device *dev, const uint8_t *bssid, bool success,
			  uint8_t device_id);

/**
 * qcom_ent_4way_hs_done - notify enterprise glue that firmware completed the
 * 4-way handshake (FOURWAY_HANDSHAKE_SUCCESS).
 *
 * Raises wifi_mgmt_raise_connect_result_event() and starts DHCP.
 * Called from station_connect_event() for EAP security types when
 * reason_code == FOURWAY_HANDSHAKE_SUCCESS.
 *
 * @iface  net_if for the station interface
 */
void qcom_ent_4way_hs_done(struct net_if *iface);

/**
 * qcom_ent_setup_supplicant - configure wpa_supplicant EAP network params.
 *
 * Called from qwifi_drv_connect() for EAP security types, before
 * qapi_WLAN_Commit().  Writes EAP method, certificates, and identity
 * into wpa_supplicant via wpa_cli commands but does NOT call select_network
 * (firmware triggers 802.11 auth+assoc directly).
 *
 * @dev     Zephyr device pointer
 * @params  Connection params (EAP security type)
 * @return  0 on success, negative on error
 */
int qcom_ent_setup_supplicant(const struct device *dev,
			      struct wifi_connect_req_params *params);

#ifdef __cplusplus
}
#endif

#endif /* QCOM_WIFI_ENTERPRISE_GLUE_H */
