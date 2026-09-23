/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef QCOM_WIFI_P2P_H_
#define QCOM_WIFI_P2P_H_

#include <stdint.h>
#include <stdbool.h>

#define QCOM_P2P_MAX_DEV_NAME_LEN   32
#define QCOM_P2P_MAC_LEN            6
#define QCOM_P2P_DEV_TYPE_LEN       8
#define QCOM_P2P_COUNTRY_LEN        3

struct qcom_p2p_params {
	uint8_t dev_addr[QCOM_P2P_MAC_LEN];
	char device_name[QCOM_P2P_MAX_DEV_NAME_LEN];
	uint8_t pri_dev_type[QCOM_P2P_DEV_TYPE_LEN];
	char country[QCOM_P2P_COUNTRY_LEN];
	uint8_t listen_reg_class;
	uint8_t listen_channel;
	uint8_t op_reg_class;
	uint8_t op_channel;
	uint16_t config_methods;
	bool p2p_6ghz_disable;
	/* Auto-authorize unauthorized PBC GO Neg Requests (see
	 * qcom_go_neg_req_rx in qcom_wifi_p2p_glue.c). Lives here rather
	 * than in the glue's private s_ctx to reuse this struct's existing
	 * tail alignment padding — struct qcom_p2p_ctx sits with zero gap
	 * before other .bss globals (see qcom_wifi_p2p_glue.c comment on
	 * s_ctx), so growing it shifts every downstream symbol and can
	 * relocate a pre-existing out-of-bounds write onto an unrelated
	 * firmware variable. Confirmed via compiled sizeof() that adding
	 * this bool here does not change sizeof(qcom_p2p_params) (58 B
	 * both before and after — the struct's 2-byte alignment already
	 * left one byte of tail padding after p2p_6ghz_disable).
	 */
	bool pbc_auto_auth;
};

/* WPS provisioning method passed to qcom_p2p_connect(). Values intentionally
 * mirror enum p2p_wps_method from hostap so the glue can forward as-is.
 */
enum qcom_p2p_wps_method {
    QCOM_P2P_WPS_NOT_READY = 0,
    QCOM_P2P_WPS_PIN_DISPLAY,
    QCOM_P2P_WPS_PIN_KEYPAD,
    QCOM_P2P_WPS_PBC,
    QCOM_P2P_WPS_NFC,
    QCOM_P2P_WPS_P2PS,
};

int qcom_p2p_enable(const struct qcom_p2p_params *params);

/* Tear down the P2P stack: cancel in-flight find / GO-neg, drop the peer
 * table, deinit hostap p2p, release the eloop thread reference. After
 * this returns the qcom_p2p_* APIs report -1 until the next enable.
 * Idempotent: calling on a disabled stack is a no-op returning 0.
 */
int qcom_p2p_disable(void);

/* Push P2P configuration updates after enable. Allowed runtime-mutable
 * fields:
 *   device_name, country, listen_reg_class, listen_channel,
 *   op_reg_class, op_channel, config_methods, pri_dev_type
 * dev_addr / p2p_6ghz_disable cannot be changed at runtime (require
 * hostap re-init). Returns 0 on success, -1 if P2P not enabled or
 * hostap rejected one of the fields. The caller is expected to have
 * populated the full struct (copy-on-edit pattern).
 */
int qcom_p2p_apply_runtime_cfg(const struct qcom_p2p_params *params);

/* Configure the P2P discovery interval. min/max are 100-TU units used
 * as the random listen-iteration duration during p2p_find. max_tu (raw
 * TUs) further caps the chosen duration; pass -1 to disable the cap.
 * Returns 0 on success, -1 if P2P is not enabled or hostap rejects.
 * Wraps p2p_set_disc_int. */
int qcom_p2p_apply_disc_int(int min_disc_int, int max_disc_int, int max_disc_tu);

int qcom_p2p_find_start(unsigned int timeout);
int qcom_p2p_find_stop(void);
/* Initiate P2P group formation with a discovered peer.
 *  - peer_mac: target P2P device address (must be in hostap peer table,
 *    i.e. previously seen by p2p find).
 *  - wps_method: PBC / PIN display / PIN keypad / etc.
 *  - go_intent: 0..15 (0 = always client, 15 = always GO).
 *  - persistent: 0 / 1 / 2 (see hostap p2p_connect doc).
 *  - auth: 0 = active connect (we send GO Neg Request),
 *          1 = authorize only (we wait for the peer to retry — used to
 *              accept a P2P-GO-NEG-REQUEST that arrived before we were
 *              ready). Mirrors wpa_cli "p2p_connect ... auth".
 * Returns 0 if hostap accepted the request (does NOT mean GO neg succeeded).
 */
int qcom_p2p_connect(const uint8_t peer_mac[QCOM_P2P_MAC_LEN],
                     enum qcom_p2p_wps_method wps_method,
                     int go_intent, int persistent, int auth);

/* Enter listen-only mode for `timeout_sec` seconds. Wraps hostap
 * p2p_listen so a peer can discover us without us actively scanning.
 * timeout_sec=0 → use hostap default (5 s). Returns 0 on success. */
int qcom_p2p_listen_start(unsigned int timeout_sec);

/* Cancel a pending GO negotiation / unauthorize the GO-neg peer and
 * stop any active find. Mirrors wpa_cli "p2p_cancel". */
int qcom_p2p_cancel(void);

/* Drop all known peers from the hostap p2p device table. Mirrors
 * wpa_cli "p2p_flush". */
int qcom_p2p_flush(void);

/* Mark a peer so that any further GO-neg request from it is rejected.
 * Mirrors wpa_cli "p2p_reject <addr>". */
int qcom_p2p_reject(const uint8_t peer_mac[QCOM_P2P_MAC_LEN]);

/* Pre-authorize a peer to invite us into a group. The next Invitation
 * Request from peer_mac (matched against either Source Address or GO
 * Device Address inside the request) will be auto-accepted by
 * qcom_invitation_process; without this, an invitation from an unknown
 * peer is deferred to the user via the P2P-INVITATION-RECEIVED log line.
 * Mirrors the auth-only flow wpa_cli exposes through "p2p_connect ... auth".
 * Authorization is one-shot: it is consumed on the first match. Pass
 * NULL or all-zero MAC to clear a previously-installed authorization.
 */
int qcom_p2p_authorize_invite(const uint8_t peer_mac[QCOM_P2P_MAC_LEN]);

/* Invitation roles for qcom_p2p_invite. */
enum qcom_p2p_invite_role {
    QCOM_P2P_INVITE_ROLE_GO        = 0, /* invite peer as client into our group */
    QCOM_P2P_INVITE_ROLE_ACTIVE_GO = 1, /* invite a client back into the active GO */
    QCOM_P2P_INVITE_ROLE_CLIENT    = 2, /* invite peer (a GO) to add us as client */
};

/* Send a P2P Invitation Request to peer.
 *  - peer_mac : target P2P device address
 *  - role     : QCOM_P2P_INVITE_ROLE_*
 *  - bssid    : group BSSID, NULL if not known
 *  - ssid     : group SSID
 *  - ssid_len : SSID length
 *  - freq     : forced channel in MHz, 0 to let hostap pick
 *  - persistent_group : 1 = reinvoke a persistent group
 * Skeleton wrapper — wire-level Invitation Request goes out, but the
 * full lifecycle (GO bring-up / persistent group reuse) isn't
 * implemented yet, so result handling is best-effort.
 */
int qcom_p2p_invite(const uint8_t peer_mac[QCOM_P2P_MAC_LEN],
                    enum qcom_p2p_invite_role role,
                    const uint8_t *bssid,
                    const uint8_t *ssid, size_t ssid_len,
                    unsigned int freq,
                    int persistent_group);

/* Peer-list inspection helpers. The caller is the qwifi shell — both
 * helpers invoke shell_print() through a small print_cb so the glue
 * does not depend on the Zephyr shell headers.
 */
typedef void (*qcom_p2p_print_cb)(void *cb_ctx, const char *fmt, ...);

int qcom_p2p_peers_dump(qcom_p2p_print_cb cb, void *cb_ctx);
int qcom_p2p_peer_dump(const uint8_t mac[QCOM_P2P_MAC_LEN],
                       qcom_p2p_print_cb cb, void *cb_ctx);

/* Feed one scan-result entry into the hostap p2p core so its peer table
 * is populated and dev_found callbacks fire. Caller is the WMI scan-result
 * event handler. ies / ies_len point at the tagged-IE region copied from
 * the original beacon/probe-response. Returns 0 on success, -1 if P2P
 * isn't enabled (call is silently ignored).
 */
int qcom_p2p_feed_bss(const uint8_t bssid[QCOM_P2P_MAC_LEN], int freq,
                      int rssi_dbm, const uint8_t *ies, size_t ies_len);

/* Mark the end of one scan run. Lets hostap process the accumulated peer
 * list and possibly schedule the next find iteration.
 */
void qcom_p2p_scan_done(void);

/* Fired by the WMI event dispatcher (wmi_send_raw_event) when a P2P
 * off-channel action-frame TX + dwell completes. `success` is 1 on TX
 * ack, 0 on failure. Drives p2p_send_action_cb through the eloop thread
 * with the four-tuple stashed by the preceding qcom_send_action call.
 * No-op if the last raw send wasn't a P2P action frame.
 */
void qcom_p2p_on_action_tx_done(int success);

/* Build the P2P IE (Capability + Device Info, optionally Ext Listen Timing)
 * for a (Re)Association Request to a P2P GO, as required by the P2P spec
 * (section 3.1.4.1 / wpas_p2p_assoc_req_ie in upstream wpa_supplicant). The
 * GO's WPS registrar parses this IE to recover our P2P Device Address and
 * matches it against the Enrollee it authorized during GO Negotiation
 * (wps_registrar_p2p_dev_addr_match); without it the address is unknown and
 * the GO treats the association as an unexpected/second PBC session,
 * replying M2D config_error=12. bssid selects which peer's go_neg record to
 * pull the P2P Device Address hint from (may be NULL). buf/len is the
 * caller's IE scratch buffer. Returns the number of bytes written, or -1 on
 * failure / if P2P isn't enabled (safe to skip appending in that case).
 */
int qcom_p2p_build_assoc_req_ie(const uint8_t bssid[QCOM_P2P_MAC_LEN],
                                uint8_t *buf, size_t len);

/* Toggle auto-authorization of unauthorized PBC GO Neg Requests (see
 * qcom_go_neg_req_rx). Defaults to enabled on every qcom_p2p_enable().
 * Disabling it falls back to the manual 'qwifi p2p connect ... pbc auth'
 * flow. Returns 0 on success, -1 if P2P is not enabled.
 */
int qcom_p2p_set_pbc_auto_auth(bool enable);

#endif /* QCOM_WIFI_P2P_H_ */
