/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: Apache-2.0
 *
 * QCC730 NAN Discovery Engine glue — MINIMAL hostap build (FR203517).
 * Bridges nan_de.c directly to QCC730 WMI without wpa_supplicant.
 */

#ifndef QCC730_NAN_DE_GLUE_H
#define QCC730_NAN_DE_GLUE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Upper API — called by qnan_shell.c.
 *
 * ssi / ssi_len  : optional Service Specific Information, may be NULL/0.
 * ttl            : publish time-to-live in seconds; 0 = transmit once.
 * unsolicited    : periodically broadcast publish SDF.
 * solicited      : reply to a received active Subscribe SDF.
 *                  (at least one of unsolicited/solicited must be true)
 * freq_list      : NULL-terminated MHz array for multi-channel rotation,
 *                  e.g. {5180, 5220, 5745, 0} for 5GHz social channels.
 *                  NULL = use 2.4GHz default {2437, 2437, 2412, 2462, 0}.
 * active         : subscribe transmits Subscribe SDFs (false = passive listen).
 * Returns >0 instance ID on success, <0 on error.
 */
int  qcc730_nan_glue_publish(const char *service_name, uint8_t srv_proto_type,
                              const uint8_t *ssi, size_t ssi_len,
                              unsigned int ttl,
                              bool unsolicited, bool solicited,
                              const int *freq_list);

int  qcc730_nan_glue_subscribe(const char *service_name, uint8_t srv_proto_type,
                               bool active, unsigned int ttl, unsigned int freq);

/*
 * Send a NAN follow-up (transmit) to a peer discovered earlier.
 * handle          : local instance id (publish_id or subscribe_id).
 * peer_addr       : peer NMI (6 bytes).
 * req_instance_id : peer instance id from discovery_result/receive.
 * ssi / ssi_len   : optional payload, may be NULL/0.
 * Returns 0 on success, <0 on error.
 */
int  qcc730_nan_glue_transmit(int handle, const uint8_t *peer_addr,
                              uint8_t req_instance_id,
                              const uint8_t *ssi, size_t ssi_len);

void qcc730_nan_glue_cancel_publish(int publish_id);

/*
 * Receive callback — fired by nan_glue_receive() when a NAN follow-up arrives.
 * Registered by the Matter WiFiPAF platform layer (FR203519) so that PASE/CASE
 * frames delivered via NAN follow-up are forwarded into the Matter event loop.
 * Called from the eloop thread — implementation must PostEventOrDie() to cross
 * into the Matter thread before touching any Matter APIs.
 * Pass NULL to unregister.
 */
typedef void (*qcc730_nan_receive_cb_t)(int id, int peer_instance_id,
                                        const uint8_t *ssi, size_t ssi_len,
                                        const uint8_t *peer_addr);

void qcc730_nan_glue_set_receive_cb(qcc730_nan_receive_cb_t cb);

/*
 * Allow WiFi connection scans: clear the NAN scan suppression flag so that
 * the WiFi stack can scan for the AP.  Must be called before initiating a
 * WiFi association (e.g. from ZephyrWifiDriver::ConnectNetwork).  Safe to
 * call even if NAN is not active.
 */
void qcc730_nan_glue_allow_wifi_scan(void);

/*
 * Re-enable NAN SDF RX after WiFi connection completes (success or failure).
 * Must be called after qcc730_nan_glue_allow_wifi_scan() so the next
 * commissioning cycle can discover subscribers again.
 */
void qcc730_nan_glue_resume_nan_rx(void);

/*
 * WMI event entry points — called from qcom_wifi_drv.c.
 * data points to the raw WMI event payload.
 */
void qcc730_nan_glue_roc_evt(void *data);
void qcc730_nan_glue_tx_status_evt(void *data);
void qcc730_nan_glue_rx_sdf_evt(void *data);

#ifdef __cplusplus
}
#endif

#endif /* QCC730_NAN_DE_GLUE_H */
