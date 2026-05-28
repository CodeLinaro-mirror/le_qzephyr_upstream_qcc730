/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * P2P glue between hostap p2p core (modules/lib/hostap/src/p2p) and the
 * QCC730 firmware. The host hostap module owns the p2p_init() / p2p_find()
 * state machine; this glue translates hostap callbacks into qapi_WLAN_P2P_*
 * WMI commands. Group-formation / PD / SD / invitation are intentionally
 * stubbed — only device discovery (find/stop_find) is wired through.
 */

#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "includes.h"
#include "common.h"
#include "eloop.h"
#include "wpa_debug.h"
#include "wpabuf.h"
#include "common/defs.h"
#include "common/ieee802_11_defs.h"

#include "p2p/p2p.h"
#include "wps/wps.h"
#include "utils/uuid.h"

#include "qcom_wifi_p2p.h"
#include "qcom_hostap_eloop.h"
#include "qapi_wlan_p2p.h"
#include "qapi_wlan_base.h"
#include "qapi_wlan_param_group.h"
#include "wlan_drv.h"
#ifdef CONFIG_WIFI_QCOM_WPS
#include "qcom_wps_glue.h"
#include <zephyr/net/net_if.h>
#endif

#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT_P2P_NO_DEBUG
#define QCOM_P2P_GLUE_LOG_LEVEL LOG_LEVEL_WRN
#else
#define QCOM_P2P_GLUE_LOG_LEVEL LOG_LEVEL_INF
#endif

LOG_MODULE_REGISTER(qcom_p2p_glue, QCOM_P2P_GLUE_LOG_LEVEL);

/* Forward declarations for posters whose definitions live alongside the
 * other WMI -> hostap event entry points at the end of this file. */
static int qcom_hostap_post_p2p_listen_started(unsigned int freq,
                                               unsigned int duration_ms);
static int qcom_hostap_post_p2p_action_tx_done(unsigned int freq,
                                               const uint8_t *dst,
                                               const uint8_t *src,
                                               const uint8_t *bssid,
                                               int success);

/*
 * All P2P-glue mutable state hangs off one file-scope context. Hostap
 * callbacks reach it through p2p_config::cb_ctx (the `void *ctx`
 * parameter); public entry points touch s_ctx by name because they run
 * on the WMI or shell task with no hostap ctx handy. WPS identity
 * strings and the social-channel list stay outside the struct — they
 * are compile-time constants, pulling them into BSS costs RAM for
 * nothing.
 */
struct qcom_p2p_tx_cache {
    /* Snapshot of the most recent P2P action TX. WMI_SEND_RAW_FRAME_EVTID
     * carries no payload, so qcom_send_action stashes the 4-tuple here
     * and qcom_p2p_on_action_tx_done feeds it into p2p_send_action_cb.
     * Single writer / single reader — both on the WMI dispatch task. */
    unsigned int freq;
    uint8_t      dst[QCOM_P2P_MAC_LEN];
    uint8_t      src[QCOM_P2P_MAC_LEN];
    uint8_t      bssid[QCOM_P2P_MAC_LEN];
    uint8_t      valid;
};

struct qcom_p2p_wps_devinfo {
    /* Handed to wps_build_probe_req_ie at every P2P scan. The char*
     * fields point into the sibling arrays below and into the file-scope
     * g_wps_* strings, which all outlive the P2P session. */
    struct wps_device_data data;
    u8    uuid[16];
    char  device_name[QCOM_P2P_MAX_DEV_NAME_LEN];
};

struct qcom_p2p_ctx {
    struct p2p_data              *handle;    /* hostap p2p handle */
    struct qcom_p2p_params        app_cfg;   /* snapshot of enable/apply cfg */
    /* Pre-authorized invite peer — mirror of wpa_s->p2p_auth_invite.
     * Zero MAC means the next invitation is deferred to the user via
     * P2P-INVITATION-RECEIVED. Set by qcom_p2p_authorize_invite,
     * one-shot cleared inside qcom_invitation_process. */
    u8                            auth_invite_peer[ETH_ALEN];
    struct qcom_p2p_wps_devinfo   wps;
    struct qcom_p2p_tx_cache      last_tx;
};

static struct qcom_p2p_ctx s_ctx;

/* WPS device-identity strings that never change at runtime. Kept at
 * file scope so wps_device_data.{manufacturer,model_name,...} can point
 * at them for the lifetime of the P2P session. */
static char g_wps_manufacturer[]  = "Qualcomm";
static char g_wps_model_name[]    = "QCC730";
static char g_wps_model_number[]  = "1.0";
static char g_wps_serial_number[] = "0";

/* Social channels in MHz. Match the wpas_p2p_scan list (we drop the
 * 60 GHz entry because qcc730 is 2.4 GHz only). Used by both
 * P2P_SCAN_SOCIAL and P2P_SCAN_SOCIAL_PLUS_ONE.
 */
static const int g_p2p_social_freqs[] = { 2412, 2437, 2462 };

/* ----------------------------- callbacks ----------------------------- */

static void qcom_p2p_debug_print(void *ctx, int level, const char *msg)
{
    (void)ctx;
    if (level >= MSG_INFO) {
        LOG_INF("%s", msg);
    } else {
        LOG_DBG("%s", msg);
    }
}

/* Build the freq list for a given P2P scan type. Mirrors the switch in
 * wpas_p2p_scan but trimmed to 2.4 GHz (qcc730 has no 5/6 GHz radio).
 *
 *   freqs_out: caller-provided buffer, must hold at least
 *              ARRAY_SIZE(g_p2p_social_freqs) + 2 ints
 *              (room for social + one extra + null terminator)
 *
 * Returns the number of populated freq slots (excluding the null
 * terminator). Unknown scan types fall through to "no freqs" which
 * tells fw to pick its own full sweep.
 */
static unsigned int qcom_p2p_build_freq_list(struct p2p_data *p2p,
                                             enum p2p_scan_type type,
                                             int freq, int *freqs_out)
{
    unsigned int n = 0;

    switch (type) {
    case P2P_SCAN_SOCIAL:
        for (size_t i = 0; i < ARRAY_SIZE(g_p2p_social_freqs); i++) {
            freqs_out[n++] = g_p2p_social_freqs[i];
        }
        break;
    case P2P_SCAN_SPECIFIC:
        if (freq > 0) {
            freqs_out[n++] = freq;
        }
        break;
    case P2P_SCAN_SOCIAL_PLUS_ONE:
        for (size_t i = 0; i < ARRAY_SIZE(g_p2p_social_freqs); i++) {
            freqs_out[n++] = g_p2p_social_freqs[i];
        }
        if (freq > 0 && p2p_supported_freq(p2p, freq)) {
            freqs_out[n++] = freq;
        }
        break;
    case P2P_SCAN_FULL:
    default:
        /* No freq list — firmware sweeps all supported 2.4 GHz channels. */
        break;
    }
    freqs_out[n] = 0; /* null terminator (matches wpa_driver_scan_params) */
    return n;
}

/* Mirrors wpas_p2p_scan: build WPS Probe Req IE + P2P IE, hand the
 * resulting IE blob (and the freq list) to the firmware-offloaded scan.
 *
 * Differences from wpas_p2p_scan, all motivated by MINIMAL+P2P_ONLY:
 *   - No struct wpa_supplicant: WPS device data lives in static globals
 *     populated by qcom_p2p_enable; UUID is generated once.
 *   - No radio_add_work / scan_work concurrency manager: the WMI dispatch
 *     queue serialises commands by itself, so a missing scan_work guard
 *     is acceptable. (A re-entrant call before the previous scan finishes
 *     is rejected by hostap p2p core anyway.)
 *   - bands is hardcoded to BAND_2_4_GHZ.
 *   - 6 GHz is stripped — qcc730 has no 6 GHz radio. include_6ghz is
 *     forwarded to fw as informational only.
 */
static int qcom_p2p_scan(void *ctx, enum p2p_scan_type type, int freq,
                         unsigned int num_req_dev_types,
                         const u8 *req_dev_types, const u8 *dev_id,
                         u16 pw_id, bool include_6ghz)
{
    struct qcom_p2p_ctx         *c = ctx;
    int                          freqs[ARRAY_SIZE(g_p2p_social_freqs) + 2];
    qapi_WLAN_P2P_Scan_Params_t  sp;
    struct wpabuf               *wps_ie = NULL;
    struct wpabuf               *ies    = NULL;
    size_t                       p2p_ielen;
    unsigned int                 num_freqs;
    qapi_Status_t                st;
    int                          rc = -1;

    if (c == NULL || c->handle == NULL) {
        return -1;
    }

    num_freqs = qcom_p2p_build_freq_list(c->handle, type, freq, freqs);

    /* Build the WPS Probe Req IE. wpas_p2p_scan flips wps->dev.p2p = 1
     * before this call so the IE is tagged for P2P; we do the same.
     */
    c->wps.data.p2p = 1;
    wps_ie = wps_build_probe_req_ie(pw_id, &c->wps.data, c->wps.uuid,
                                    WPS_REQ_ENROLLEE,
                                    num_req_dev_types, req_dev_types);
    if (wps_ie == NULL) {
        LOG_ERR("p2p_scan: wps_build_probe_req_ie failed");
        goto out;
    }

    /* Append the P2P IE. p2p_scan_ie_buf_len gives us the upper bound it
     * may write so we can size the wpabuf once.
     */
    p2p_ielen = p2p_scan_ie_buf_len(c->handle);
    ies = wpabuf_alloc(wpabuf_len(wps_ie) + p2p_ielen);
    if (ies == NULL) {
        LOG_ERR("p2p_scan: wpabuf_alloc(%zu) failed",
                wpabuf_len(wps_ie) + p2p_ielen);
        goto out;
    }
    wpabuf_put_buf(ies, wps_ie);
    p2p_scan_ie(c->handle, ies, dev_id, BAND_2_4_GHZ);

    /* Hand the assembled scan parameters to the firmware. The disc_type
     * field is a legacy hint — kept consistent with the freq list so a
     * fw that ignores num_freqs still does the right sweep.
     */
    memset(&sp, 0, sizeof(sp));
    sp.disc_type        = (type == P2P_SCAN_SOCIAL ||
                           type == P2P_SCAN_SOCIAL_PLUS_ONE)
                          ? QAPI_WLAN_P2P_DISC_ONLY_SOCIAL_E
                          : QAPI_WLAN_P2P_DISC_START_WITH_FULL_E;
    sp.timeout_In_Secs  = 0;       /* hostap eloop drives the timer */
    sp.p2p_probe        = 1;
    sp.include_6ghz     = include_6ghz ? 1 : 0;
    sp.freqs            = (num_freqs > 0) ? freqs : NULL;
    sp.extra_ies        = wpabuf_head(ies);
    sp.extra_ies_len    = wpabuf_len(ies);
    sp.ssid             = (const uint8_t *)P2P_WILDCARD_SSID;
    sp.ssid_len         = P2P_WILDCARD_SSID_LEN;

    st = qapi_WLAN_P2P_Find(0, &sp);
    rc = (st == QAPI_OK) ? 0 : -1;

out:
    wpabuf_free(wps_ie);
    wpabuf_free(ies);
    return rc;
}

/* Convert a 2.4 GHz frequency in MHz to an IEEE channel number for
 * qapi_WLAN_Raw_Send. Returns 0 (current channel) for any freq we
 * can't translate — Raw_Send rejects channel==0 unconditionally, so
 * the caller falls back to the home channel via the path that ignores
 * the channel field when STA is connected. */
/* Convert a P2P frequency in MHz to the corresponding IEEE channel
 * number used by qapi_WLAN_Raw_Send. Supports 2.4 GHz (channels 1..14)
 * and 5 GHz (channels 36..165 across the standard UNII bands). 6 GHz
 * is intentionally not handled here — QCC730 hardware supports 6 GHz
 * but P2P discovery is restricted to 2.4 GHz social channels by spec,
 * and 5 GHz is enough for the post-formation operating channel.
 * Returns 0 for any freq we can't translate; Raw_Send rejects channel==0
 * unconditionally so the caller falls back to home-channel TX. */
static uint32_t qcom_p2p_freq_to_chan(unsigned int freq)
{
    /* 2.4 GHz: ch 1..13 = 2412..2472 step 5 MHz; ch 14 = 2484 */
    if (freq >= 2412 && freq <= 2472 && ((freq - 2412) % 5) == 0) {
        return (freq - 2412) / 5 + 1;
    }
    if (freq == 2484) {
        return 14;
    }
    /* 5 GHz: ch N = 5000 + 5*N for N in {36..64, 100..144, 149..165} */
    if (freq >= 5170 && freq <= 5825 && ((freq - 5000) % 5) == 0) {
        unsigned int ch = (freq - 5000) / 5;
        if ((ch >= 36 && ch <= 64) ||
            (ch >= 100 && ch <= 144) ||
            (ch >= 149 && ch <= 165)) {
            return ch;
        }
    }
    return 0;
}

void qcom_p2p_on_action_tx_done(int success)
{
    struct qcom_p2p_tx_cache *t = &s_ctx.last_tx;

    if (!t->valid) {
        return;
    }
    t->valid = 0;
    (void)qcom_hostap_post_p2p_action_tx_done(t->freq, t->dst, t->src,
                                              t->bssid, success);
}

int qcom_p2p_build_assoc_req_ie(const uint8_t bssid[QCOM_P2P_MAC_LEN],
                                uint8_t *buf, size_t len)
{
    if (s_ctx.handle == NULL || buf == NULL || len == 0) {
        return -1;
    }
    return p2p_assoc_req_ie(s_ctx.handle, bssid, buf, len,
                            1 /* p2p_group: associating with a P2P GO */,
                            NULL /* no reassembled scan-result P2P IE */);
}

static int qcom_send_action(void *ctx, unsigned int freq, const u8 *dst,
                            const u8 *src, const u8 *bssid, const u8 *buf,
                            size_t len, unsigned int wait_time, int *scheduled)
{
    struct qcom_p2p_ctx *c = ctx;

    /* Build the 802.11 mgmt action frame (24B hdr + body) into a
     * USER_DEFINED buffer and hand it to the P2P-aware TX path. That
     * path (qapi_WLAN_P2P_Send_Action) tells the fw to off-channel-TX
     * on `freq`, dwell `wait_time` so the peer's Response can be RX'd
     * on that channel, then restore the previous channel and fire
     * WMI_SEND_RAW_FRAME_EVTID — which drives our tx_done callback. */
    qapi_WLAN_P2P_Send_Action_Params_t rp;
    uint8_t                            frame[256];

    if (buf == NULL || len == 0 || len > sizeof(frame) - 24) {
        LOG_WRN("send_action: bad len=%zu", len);
        if (scheduled) *scheduled = 0;
        return -1;
    }

    memset(&rp, 0, sizeof(rp));
    memset(frame, 0, sizeof(frame));

    /* 802.11 ACTION mgmt header: FC=0xd0 (subtype=ACTION), duration=0 */
    frame[0] = 0xd0;
    frame[1] = 0x00;
    frame[2] = 0x00; frame[3] = 0x00;
    memcpy(&frame[4],  dst,   6);   /* addr1 = DA */
    memcpy(&frame[10], src,   6);   /* addr2 = SA */
    memcpy(&frame[16], bssid, 6);   /* addr3 = BSSID */
    frame[22] = 0; frame[23] = 0;   /* seq — fw fills */
    memcpy(&frame[24], buf, len);

    rp.freq         = freq;
    rp.wait_time_ms = wait_time ? wait_time : 500;
    rp.num_tries    = 7;
    memcpy(rp.dst,   dst,   6);
    memcpy(rp.src,   src,   6);
    memcpy(rp.bssid, bssid, 6);
    rp.data         = frame;
    rp.data_len     = (uint32_t)(24 + len);

    /* Stash the frame context so WMI_SEND_RAW_FRAME_EVTID (fired by fw
     * when dwell ends) can post p2p_send_action_cb with the right args. */
    c->last_tx.freq = freq;
    memcpy(c->last_tx.dst,   dst,   QCOM_P2P_MAC_LEN);
    memcpy(c->last_tx.src,   src,   QCOM_P2P_MAC_LEN);
    memcpy(c->last_tx.bssid, bssid, QCOM_P2P_MAC_LEN);
    c->last_tx.valid = 1;

    qapi_Status_t st = qapi_WLAN_P2P_Send_Action(0, &rp);
    if (st != QAPI_OK) {
        /* TX submission failed — fw won't fire EVTID, so invalidate the
         * stash and drive tx_done immediately with failure. */
        c->last_tx.valid = 0;
        (void)qcom_hostap_post_p2p_action_tx_done(freq, dst, src, bssid, 0);
    }

    if (scheduled) *scheduled = 0;
    return (st == QAPI_OK) ? 0 : -1;
}

static void qcom_send_action_done(void *ctx) { (void)ctx; }

/* Listen state — driven by hostap p2p_state_machine which alternates
 * SEARCH (cfg->p2p_scan) and LISTEN_ONLY (cfg->start_listen). The
 * firmware can't actually receive Probe Reqs in this build profile yet,
 * but it does run a software timer matching `duration` and posts
 * WMI_P2P_LISTEN_DONE_EVTID back, which is enough to keep hostap's
 * find/listen alternation ticking. The probe_resp_ie blob is forwarded
 * to fw so when a real ROC RX path lands the IE template is already in
 * place.
 */
static int qcom_start_listen(void *ctx, unsigned int freq,
                             unsigned int duration,
                             const struct wpabuf *probe_resp_ie)
{
    qapi_Status_t  st;
    const u8      *ies = NULL;
    size_t         ies_len = 0;

    (void)ctx;

    if (freq == 0 || duration == 0) {
        return -1;
    }
    /* hostap caps duration at p2p_config::max_listen (5 s by default).
     * Clamp to uint16_t just in case a very large value sneaks through.
     */
    if (duration > 0xffff) {
        duration = 0xffff;
    }
    if (probe_resp_ie != NULL) {
        ies     = wpabuf_head(probe_resp_ie);
        ies_len = wpabuf_len(probe_resp_ie);
    }

    st = qapi_WLAN_P2P_Listen(0, (uint16_t)freq, (uint16_t)duration,
                              ies, ies_len);
    if (st != QAPI_OK) {
        return -1;
    }
    /* Tell hostap the driver actually entered Listen state. fw arms its
     * dwell timer synchronously inside qapi_WLAN_P2P_Listen, so by the
     * time we return here the listen is in progress. Posting via the
     * eloop fifo (not a direct call) so p2p_listen_cb runs on the
     * hostap eloop thread with the glue lock held. */
    (void)qcom_hostap_post_p2p_listen_started(freq, duration);
    return 0;
}

static void qcom_stop_listen(void *ctx)
{
    (void)ctx;
    (void)qapi_WLAN_P2P_Cancel_Listen(0);
}

static void qcom_dev_found(void *ctx, const u8 *addr,
                           const struct p2p_peer_info *info,
                           int new_device)
{
    (void)ctx; (void)new_device;

    LOG_WRN("P2P-DEVICE-FOUND %02x:%02x:%02x:%02x:%02x:%02x name=\"%s\"",
            addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],
            info->device_name);
}

static void qcom_dev_lost(void *ctx, const u8 *dev_addr)
{
    (void)ctx; (void)dev_addr;
}

static void qcom_find_stopped(void *ctx) { (void)ctx; }

static void qcom_go_neg_completed(void *ctx, struct p2p_go_neg_results *res)
{
    struct qcom_p2p_ctx *c = ctx;
    qapi_WLAN_WPS_Credentials_t creds;
    uint16_t channel = 0;

    if (c == NULL || res == NULL) {
        return;
    }

    LOG_WRN("P2P-GO-NEG-COMPLETED status=%d role=%s freq=%d ssid_len=%zu peer="
            "%02x:%02x:%02x:%02x:%02x:%02x",
            res->status,
            res->role_go ? "GO" : "CLIENT",
            res->freq, res->ssid_len,
            res->peer_interface_addr[0], res->peer_interface_addr[1],
            res->peer_interface_addr[2], res->peer_interface_addr[3],
            res->peer_interface_addr[4], res->peer_interface_addr[5]);

    if (res->status != 0) {
        LOG_WRN("GO neg failed (status=%d) — abort provisioning", res->status);
        return;
    }
    if (res->role_go) {
        /* We're the GO — would need to start a SoftAP-mode BSS with the
         * negotiated SSID/passphrase. Not implemented; current
         * deployments have us be a client. */
        LOG_WRN("local role=GO not implemented yet");
        return;
    }

    /* Convert P2P operating freq (MHz) → IEEE channel for the WPS profile.
     * Reuses qcom_p2p_freq_to_chan which covers 2.4 GHz and 5 GHz. */
    channel = (uint16_t)qcom_p2p_freq_to_chan(res->freq);

    /* Halt discovery AND listen on BOTH host and fw before entering WPS
     * provisioning. Otherwise the fw keeps broadcasting the wildcard
     * "DIRECT-" P2P Probe Request (with the P2P + PBC WSC IEs installed by
     * p2p_fw_find) on social channels during our EAP-WSC session, and the
     * GO's hostapd interprets that as a *second* PBC enrollee and replies
     * M2D config_error=12 (Multiple PBC Detected).  qapi_WLAN_P2P_Stop_Find
     * triggers p2p_fw_stop_find in fw, which cancels the in-flight scan
     * and clears the PROBE_REQ appie so no more DIRECT- probes go out.
     *
     * qapi_WLAN_P2P_Stop_Find is fire-and-forget (WMI_P2P_STOP_FIND_CMDID
     * is queued via wmi_cmd_send and processed asynchronously by the WMI
     * worker task). If fw is mid-dwell when GO Neg Confirm lands, the
     * dwell's probe request has already gone out on air before
     * p2p_fw_stop_find() runs — dc_cancel_scan only stops *future* dwells.
     * Sleep one dwell period here (eloop thread context, blocking is fine)
     * so that stray probe is drained before we open-associate and start
     * EAP-WSC; otherwise the GO can still see it inside its PBC overlap
     * detection window and reject M2 with config_error=12.
     *
     * Separately: hostap's p2p_listen_in_find() puts us into a short
     * P2P_CONNECT_LISTEN window after every GO Neg Request retry while
     * waiting for a response — its Probe Response IE advertises our
     * go_neg_peer's wps_method (PBC) via p2p_build_probe_resp_ies(). If a
     * GO Neg Confirm lands mid-dwell, that dwell is NOT torn down by
     * hostap: p2p_clear_timeout() (called right before go_neg_completed)
     * only cancels the eloop timeout, and the public p2p_stop_listen() API
     * is a no-op outside P2P_LISTEN_ONLY state, so it never fires here.
     * The dwell then runs to completion in parallel with our WPS M1..M8
     * exchange, keeping a PBC-tagged Probe Response live on air right
     * through the GO's overlap-detection window. Cancel it directly via
     * the fw API (bypassing hostap's state-gated p2p_stop_listen). */
    p2p_stop_find(c->handle);
    (void)qapi_WLAN_P2P_Stop_Find(0);
    (void)qapi_WLAN_P2P_Cancel_Listen(0);
    k_msleep(50);

#ifdef CONFIG_WIFI_QCOM_WPS
    if (res->wps_method != WPS_PBC) {
        LOG_WRN("P2P-GO-NEG: wps_method=%d unsupported (only PBC wired)",
                res->wps_method);
        return;
    }

    struct net_if *iface = net_if_get_first_wifi();
    if (!iface) {
        LOG_ERR("P2P-GO-NEG: no wifi iface — cannot start WPS enrollee");
        return;
    }
    const struct device *dev = net_if_get_device(iface);

    int rc = qcom_wps_start_from_bssid(dev, res->peer_interface_addr,
                                        res->ssid, (uint8_t)res->ssid_len,
                                        channel);
    if (rc) {
        LOG_ERR("P2P-GO-NEG: qcom_wps_start_from_bssid failed (%d)", rc);
    } else {
        LOG_WRN("P2P-GO-NEG: WPS enrollee started (bssid=%02x:%02x:%02x:%02x:"
                "%02x:%02x ch=%u ssid=%.*s)",
                res->peer_interface_addr[0], res->peer_interface_addr[1],
                res->peer_interface_addr[2], res->peer_interface_addr[3],
                res->peer_interface_addr[4], res->peer_interface_addr[5],
                channel, (int)res->ssid_len, (const char *)res->ssid);
    }
    (void)creds;
#else
    memset(&creds, 0, sizeof(creds));
    creds.ssid_Length = (uint8_t)res->ssid_len;
    if (res->ssid_len > 0 && res->ssid_len <= sizeof(creds.ssid)) {
        memcpy(creds.ssid, res->ssid, res->ssid_len);
    }
    memcpy(creds.mac_Addr, res->peer_interface_addr, 6);
    creds.ap_Channel = channel;
    LOG_WRN("P2P-GO-NEG: WPS provisioning skipped (CONFIG_WIFI_QCOM_WPS=n);"
            " staged ssid=%.*s ch=%u peer_iface=%02x:%02x:%02x:%02x:%02x:%02x",
            (int)creds.ssid_Length, (const char *)creds.ssid,
            (unsigned)creds.ap_Channel,
            creds.mac_Addr[0], creds.mac_Addr[1], creds.mac_Addr[2],
            creds.mac_Addr[3], creds.mac_Addr[4], creds.mac_Addr[5]);
    (void)creds;
#endif
}

/* Deferred payload for qcom_go_neg_authorize_deferred — see the comment
 * on qcom_go_neg_req_rx for why the p2p_authorize() call can't happen
 * inline inside that callback. */
struct qcom_deferred_auth {
    u8 peer_addr[ETH_ALEN];
    u8 go_intent;
};

static void qcom_go_neg_authorize_deferred(void *eloop_ctx, void *user_ctx)
{
    struct qcom_deferred_auth *d = eloop_ctx;

    (void)user_ctx;
    (void)qapi_WLAN_P2P_Stop_Find(0);
    int rc = p2p_connect(s_ctx.handle, d->peer_addr, WPS_PBC, 0,
                         s_ctx.app_cfg.dev_addr, 0, 0, NULL, 0, 0, 0, 0);
    LOG_WRN("P2P-GO-NEG-REQUEST %02x:%02x:%02x:%02x:%02x:%02x"
            " -- deferred auto-connect PBC (rc=%d)",
            d->peer_addr[0], d->peer_addr[1], d->peer_addr[2],
            d->peer_addr[3], d->peer_addr[4], d->peer_addr[5], rc);
    k_free(d);
}

static void qcom_go_neg_req_rx(void *ctx, const u8 *src, u16 dev_passwd_id,
                               u8 go_intent)
{
    const char *method;

    (void)ctx;
    /* Mirrors wpa_supplicant's P2P-GO-NEG-REQUEST event. Fires when a
     * peer sent us a GO Neg Request before we authorized them, so
     * hostap auto-replied with status=1 ("info currently unavailable"
     * / not ready) and is now waiting for the user to authorize. The
     * peer will retry, and once dev->wps_method is set (via
     * p2p_authorize) the next GO Neg Req goes through the full
     * negotiation.
     *
     * PBC has no secret to enter, so by default there is nothing for a
     * user to authorize — auto-authorize so the peer's (short) GO Neg
     * retry window doesn't expire while waiting on a human. Toggle via
     * 'qwifi p2p set pbc_auto_auth 0|1' to fall back to the manual
     * 'qwifi p2p connect ... pbc auth' path. PIN/display always stay
     * manual since they need a human to read/enter the PIN regardless.
     *
     * The flag lives in s_ctx.app_cfg (struct qcom_p2p_params), not as
     * a new top-level s_ctx field — see that struct's doc comment for
     * why: s_ctx sits with zero .bss padding before unrelated globals
     * (g_he_refcnt etc.), so growing it shifts every downstream symbol
     * and can relocate a pre-existing out-of-bounds write elsewhere in
     * the codebase onto an unrelated firmware variable.
     *
     * The p2p_authorize() call itself is deferred to the next eloop
     * iteration (eloop_register_timeout with a 0 delay) rather than run
     * inline here. This callback fires synchronously from inside
     * p2p_process_go_neg_req(), *before* that function builds and sends
     * the GO Negotiation Response for this (still-unauthorized) request
     * — p2p_build_go_neg_resp() reads dev->wps_method to pick the WSC
     * IE's Device Password ID. Calling p2p_authorize() inline would set
     * dev->wps_method = WPS_PBC right before that read, so the failure
     * Response would carry DEV_PW_PUSHBUTTON instead of the DEV_PW_DEFAULT
     * a peer expects on an "unavailable" reply — deferring keeps this
     * Response bit-for-bit identical to upstream wpa_supplicant's
     * (unauthorized) reply, while still authorizing in time for the
     * peer's retry.
     */
    if (dev_passwd_id == DEV_PW_PUSHBUTTON && s_ctx.app_cfg.pbc_auto_auth) {
        struct qcom_deferred_auth *d = k_malloc(sizeof(*d));
        if (d) {
            memcpy(d->peer_addr, src, ETH_ALEN);
            d->go_intent = go_intent;
            eloop_register_timeout(0, 0, qcom_go_neg_authorize_deferred, d,
                                   NULL);
        }
        LOG_WRN("P2P-GO-NEG-REQUEST %02x:%02x:%02x:%02x:%02x:%02x"
                " dev_passwd_id=%u go_intent=%u -- auto-authorize PBC %s",
                src[0], src[1], src[2], src[3], src[4], src[5],
                (unsigned)dev_passwd_id, (unsigned)go_intent,
                d ? "scheduled" : "failed (no mem)");
        return;
    }

    switch (dev_passwd_id) {
    case DEV_PW_PUSHBUTTON:
        method = "pbc";
        break;
    case DEV_PW_REGISTRAR_SPECIFIED:
        method = "display";
        break;
    default:
        method = "pin <PIN>";
        break;
    }

    LOG_WRN("P2P-GO-NEG-REQUEST %02x:%02x:%02x:%02x:%02x:%02x"
            " dev_passwd_id=%u go_intent=%u"
            " -- run: qwifi p2p connect %02x:%02x:%02x:%02x:%02x:%02x %s auth",
            src[0], src[1], src[2], src[3], src[4], src[5],
            (unsigned)dev_passwd_id, (unsigned)go_intent,
            src[0], src[1], src[2], src[3], src[4], src[5], method);
}

static int qcom_send_probe_resp(void *ctx, const struct wpabuf *buf,
                                unsigned int freq)
{
    (void)ctx; (void)buf; (void)freq;
    return -1;
}

static void qcom_sd_request(void *ctx, int freq, const u8 *sa,
                            u8 dialog_token, u16 update_indic,
                            const u8 *tlvs, size_t tlvs_len)
{
    (void)ctx; (void)freq; (void)sa; (void)dialog_token;
    (void)update_indic; (void)tlvs; (void)tlvs_len;
}

static void qcom_sd_response(void *ctx, const u8 *sa, u16 update_indic,
                             const u8 *tlvs, size_t tlvs_len)
{
    (void)ctx; (void)sa; (void)update_indic; (void)tlvs; (void)tlvs_len;
}

static void qcom_prov_disc_req(void *ctx, const u8 *peer, u16 config_methods,
                               const u8 *dev_addr, const u8 *pri_dev_type,
                               const char *dev_name, u16 supp_config_methods,
                               u8 dev_capab, u8 group_capab,
                               const u8 *group_id, size_t group_id_len)
{
    (void)ctx; (void)dev_addr;
    (void)pri_dev_type; (void)dev_name; (void)supp_config_methods;
    (void)dev_capab; (void)group_capab; (void)group_id; (void)group_id_len;
    LOG_WRN("P2P-PROV-DISC-REQ %02x:%02x:%02x:%02x:%02x:%02x config_methods=0x%x",
            peer[0], peer[1], peer[2], peer[3], peer[4], peer[5],
            (unsigned)config_methods);
}

static void qcom_prov_disc_resp(void *ctx, const u8 *peer, u16 config_methods)
{
    (void)ctx; (void)peer; (void)config_methods;
    LOG_WRN("P2P-PROV-DISC-RESP %02x:%02x:%02x:%02x:%02x:%02x config_methods=0x%x",
            peer[0], peer[1], peer[2], peer[3], peer[4], peer[5],
            (unsigned)config_methods);
}

static void qcom_prov_disc_fail(void *ctx, const u8 *peer,
                                enum p2p_prov_disc_status status,
                                u32 adv_id, const u8 *adv_mac,
                                const char *deferred_session_resp)
{
    (void)ctx; (void)peer; (void)status;
    (void)adv_id; (void)adv_mac; (void)deferred_session_resp;
}

/* Decide whether to accept an Invitation Request from `sa`. Modeled on
 * wpas_invitation_process (p2p_supplicant.c), trimmed to the GC-only
 * subset this FR implements:
 *
 *   - Persistent-group reinvocation: we have no on-device store of past
 *     groups, so we cannot honour a reinvocation request. Reject with
 *     P2P_SC_FAIL_UNKNOWN_GROUP. (wpa_s walks wpa_s->conf->ssid here.)
 *
 *   - Active-group invitation (peer is already a GO and wants us to join
 *     as GC): we accept iff the user has pre-authorized this peer via
 *     qcom_p2p_authorize_invite (exposes the same hook wpa_cli uses with
 *     "p2p_connect ... auth"). Without pre-authorization we return
 *     P2P_SC_FAIL_INFO_CURRENTLY_UNAVAILABLE so hostap proceeds to fire
 *     invitation_received with status=1 — the user can then authorize
 *     and the peer will retry.
 *
 *   - We always set *go=0: the GC role is fixed for this FR.
 *   - We don't overwrite *force_freq: hostap's preferred channel logic
 *     in wpa_supplicant depends on multi-channel concurrency state we
 *     don't have on QCC730.
 */
static u8 qcom_invitation_process(void *ctx, const u8 *sa, const u8 *bssid,
                                  const u8 *go_dev_addr, const u8 *ssid,
                                  size_t ssid_len, int *go,
                                  u8 *group_bssid, int *force_freq,
                                  int persistent_group,
                                  const struct p2p_channels *channels,
                                  int dev_pw_id)
{
    struct qcom_p2p_ctx *c = ctx;
    (void)bssid; (void)group_bssid;
    (void)force_freq; (void)channels; (void)dev_pw_id;

    if (c == NULL) {
        return P2P_SC_FAIL_INFO_CURRENTLY_UNAVAILABLE;
    }

    if (go) {
        *go = 0; /* GC-only build */
    }

    if (persistent_group) {
#ifdef CONFIG_WIFI_QCOM_WPS
        if (c->app_cfg.pbc_auto_auth &&
            qcom_wps_has_persistent_cred(sa, ssid, (uint8_t)ssid_len)) {
            LOG_WRN("P2P invitation: persistent — accepting "
                    "%02x:%02x:%02x:%02x:%02x:%02x ssid=\"%.*s\"",
                    sa[0], sa[1], sa[2], sa[3], sa[4], sa[5],
                    (int)ssid_len, (const char *)ssid);
            if (go) *go = 0;
            return P2P_SC_SUCCESS;
        }
#endif
        LOG_WRN("P2P invitation: persistent — no stored cred "
                "%02x:%02x:%02x:%02x:%02x:%02x ssid=\"%.*s\"",
                sa[0], sa[1], sa[2], sa[3], sa[4], sa[5],
                (int)ssid_len, (const char *)ssid);
        return P2P_SC_FAIL_INFO_CURRENTLY_UNAVAILABLE;
    }

    /* Auto-accept if pbc_auto_auth is on (covers both persistent and
     * active-group invitations), or if the user pre-authorized this peer. */
    if (c->app_cfg.pbc_auto_auth ||
        (!is_zero_ether_addr(c->auth_invite_peer) &&
         (ether_addr_equal(sa, c->auth_invite_peer) ||
          (go_dev_addr && ether_addr_equal(go_dev_addr, c->auth_invite_peer))))) {
        LOG_WRN("P2P invitation: auto-accepting %02x:%02x:%02x:%02x:%02x:%02x"
                " ssid=\"%.*s\"",
                sa[0], sa[1], sa[2], sa[3], sa[4], sa[5],
                (int)ssid_len, (const char *)ssid);
        memset(c->auth_invite_peer, 0, ETH_ALEN);
        return P2P_SC_SUCCESS;
    }

    LOG_WRN("P2P invitation: %02x:%02x:%02x:%02x:%02x:%02x not pre-authorized"
            " ssid=\"%.*s\"",
            sa[0], sa[1], sa[2], sa[3], sa[4], sa[5],
            (int)ssid_len, (const char *)ssid);
    return P2P_SC_FAIL_INFO_CURRENTLY_UNAVAILABLE;
}

/* Mirror of wpas_invitation_received: hostap has already replied with
 * the status returned by qcom_invitation_process (or with REQ_RECEIVED
 * if we hadn't installed a process callback). This is the place where
 * wpa_supplicant either kicks off the group join (status==0) or just
 * surfaces the request to the user (status==1).
 *
 * We don't yet have the GC join path implemented (assoc + 4-way + DHCP
 * — see FR open items #2 / #3 / #4). For now this hook is informational:
 * we log a stable line the host application can grep so it knows the
 * invitation was processed and a follow-up join would need to happen.
 */
static void qcom_invitation_received(void *ctx, const u8 *sa, const u8 *bssid,
                                     const u8 *ssid, size_t ssid_len,
                                     const u8 *go_dev_addr, u8 status,
                                     int op_freq)
{
    struct qcom_p2p_ctx *c = ctx;

    if (status == P2P_SC_SUCCESS) {
        const u8 *go_bssid = bssid ? bssid : sa;
        uint16_t channel = (uint16_t)qcom_p2p_freq_to_chan((unsigned int)op_freq);

        LOG_WRN("P2P-INVITATION-ACCEPTED sa=%02x:%02x:%02x:%02x:%02x:%02x"
                " op_freq=%d ch=%u ssid=\"%.*s\"",
                sa[0], sa[1], sa[2], sa[3], sa[4], sa[5],
                op_freq, channel, (int)ssid_len, (const char *)ssid);

        p2p_stop_find(c->handle);
        (void)qapi_WLAN_P2P_Stop_Find(0);
        (void)qapi_WLAN_P2P_Cancel_Listen(0);
        k_msleep(50);

#ifdef CONFIG_WIFI_QCOM_WPS
        {
            int rc = qcom_wps_connect_persistent(go_bssid,
                                                 ssid, (uint8_t)ssid_len,
                                                 channel);
            if (rc == 0) {
                LOG_WRN("P2P-INVITE: persistent PSK connect (bssid=%02x:%02x:"
                        "%02x:%02x:%02x:%02x ch=%u)",
                        go_bssid[0], go_bssid[1], go_bssid[2],
                        go_bssid[3], go_bssid[4], go_bssid[5], channel);
                return;
            }
        }
#endif
        LOG_WRN("P2P-INVITE: no cred, initiating GO Neg to "
                "%02x:%02x:%02x:%02x:%02x:%02x",
                sa[0], sa[1], sa[2], sa[3], sa[4], sa[5]);
        (void)p2p_connect(c->handle, sa, WPS_PBC, 0,
                          c->app_cfg.dev_addr, 0, 0, NULL, 0, 0, 0, 0);
        return;
    }

    if (status == P2P_SC_FAIL_INFO_CURRENTLY_UNAVAILABLE) {
        LOG_WRN("P2P-INVITE: no cred, fallback GO Neg to "
                "%02x:%02x:%02x:%02x:%02x:%02x",
                sa[0], sa[1], sa[2], sa[3], sa[4], sa[5]);
        p2p_stop_find(c->handle);
        (void)qapi_WLAN_P2P_Stop_Find(0);
        (void)qapi_WLAN_P2P_Cancel_Listen(0);
        k_msleep(50);
        (void)p2p_connect(c->handle, sa, WPS_PBC, 0,
                          c->app_cfg.dev_addr, 0, 0, NULL, 0, 0, 0, 0);
        return;
    }

    /* Any other status code is a hostap-driven rejection. */
    LOG_INF("P2P-INVITATION-REJECTED sa=%02x:%02x:%02x:%02x:%02x:%02x"
            " status=%u",
            sa[0], sa[1], sa[2], sa[3], sa[4], sa[5], (unsigned)status);
}

/* Mirror of wpas_invitation_result: result of an Invitation we sent out
 * via qcom_p2p_invite. Status comes from the peer's Invitation Response
 * (0 = accepted, non-zero = rejected, -1 = TX/timeout). On accept the
 * supplicant kicks off the group; we just log for now until the GC join
 * path lands.
 */
static void qcom_invitation_result(void *ctx, int status, const u8 *bssid,
                                   const struct p2p_channels *channels,
                                   const u8 *peer, int neg_freq,
                                   int peer_oper_freq)
{
    (void)ctx; (void)channels;

    int freq = neg_freq > 0 ? neg_freq : peer_oper_freq;

    if (status == P2P_SC_SUCCESS) {
        LOG_INF("P2P-INVITATION-RESULT status=success"
                " peer=%02x:%02x:%02x:%02x:%02x:%02x freq=%d bssid=%s"
                " [TODO: GC group join not implemented]",
                peer ? peer[0] : 0, peer ? peer[1] : 0, peer ? peer[2] : 0,
                peer ? peer[3] : 0, peer ? peer[4] : 0, peer ? peer[5] : 0,
                freq, bssid ? "set" : "(none)");
        return;
    }

    if (status < 0) {
        LOG_WRN("P2P-INVITATION-RESULT status=tx-fail/timeout"
                " peer=%02x:%02x:%02x:%02x:%02x:%02x",
                peer ? peer[0] : 0, peer ? peer[1] : 0, peer ? peer[2] : 0,
                peer ? peer[3] : 0, peer ? peer[4] : 0, peer ? peer[5] : 0);
        return;
    }

    LOG_INF("P2P-INVITATION-RESULT status=%d (rejected)"
            " peer=%02x:%02x:%02x:%02x:%02x:%02x freq=%d",
            status,
            peer ? peer[0] : 0, peer ? peer[1] : 0, peer ? peer[2] : 0,
            peer ? peer[3] : 0, peer ? peer[4] : 0, peer ? peer[5] : 0,
            freq);
}

static int qcom_get_noa(void *ctx, const u8 *interface_addr, u8 *buf,
                        size_t buf_len)
{
    (void)ctx; (void)interface_addr; (void)buf; (void)buf_len;
    return 0;
}

static int qcom_go_connected(void *ctx, const u8 *dev_addr)
{
    (void)ctx; (void)dev_addr;
    return 0;
}

static void qcom_presence_resp(void *ctx, const u8 *src, u8 status,
                               const u8 *noa, size_t noa_len)
{
    (void)ctx; (void)src; (void)status; (void)noa; (void)noa_len;
}

static int qcom_is_concurrent_session_active(void *ctx)
{
    (void)ctx; return 0;
}

static int qcom_p2p_in_progress(void *ctx)
{
    (void)ctx; return 0;
}

static int qcom_get_persistent_group(void *ctx, const u8 *addr,
                                     const u8 *ssid, size_t ssid_len,
                                     u8 *go_dev_addr, u8 *ret_ssid,
                                     size_t *ret_ssid_len,
                                     u8 *intended_iface_addr)
{
    (void)ctx; (void)addr; (void)ssid; (void)ssid_len;
    (void)go_dev_addr; (void)ret_ssid; (void)ret_ssid_len;
    (void)intended_iface_addr;
    return 0;
}

static int qcom_get_go_info(void *ctx, u8 *intended_addr, u8 *ssid,
                            size_t *ssid_len, int *group_iface, unsigned int *freq)
{
    (void)ctx; (void)intended_addr; (void)ssid; (void)ssid_len;
    (void)group_iface; (void)freq;
    return 0;
}

static int qcom_remove_stale_groups(void *ctx, const u8 *peer, const u8 *go,
                                    const u8 *ssid, size_t ssid_len)
{
    (void)ctx; (void)peer; (void)go; (void)ssid; (void)ssid_len;
    return 0;
}

static void qcom_p2ps_prov_complete(void *ctx, u8 status, const u8 *dev,
                                    const u8 *adv_mac, const u8 *ses_mac,
                                    const u8 *grp_mac, u32 adv_id, u32 ses_id,
                                    u8 conncap, int passwd_id, const u8 *persist_ssid,
                                    size_t persist_ssid_size, int response_done,
                                    int prov_start, const char *session_info,
                                    const u8 *feat_cap, size_t feat_cap_len,
                                    unsigned int freq, const u8 *group_ssid,
                                    size_t group_ssid_len)
{
    (void)ctx; (void)status; (void)dev; (void)adv_mac; (void)ses_mac;
    (void)grp_mac; (void)adv_id; (void)ses_id; (void)conncap;
    (void)passwd_id; (void)persist_ssid; (void)persist_ssid_size;
    (void)response_done; (void)prov_start; (void)session_info;
    (void)feat_cap; (void)feat_cap_len; (void)freq;
    (void)group_ssid; (void)group_ssid_len;
}

static int qcom_prov_disc_resp_cb(void *ctx)
{
    (void)ctx;
    return 0;
}

static int qcom_p2p_get_pref_freq_list(void *ctx, int go,
                                       unsigned int *len,
                                       struct weighted_pcl *freq_list)
{
    (void)ctx; (void)go; (void)len; (void)freq_list;
    return -1;
}

/* --------------------- channel list bootstrap ----------------------- */

static void qcom_p2p_setup_channels(struct p2p_channels *out)
{
    /* Channel list advertised in P2P GO Neg / Invitation / probe-resp.
     * hostap intersects this with the peer's list to pick the operating
     * channel after group formation.
     *
     * Class 81 (2.4 GHz GLOBAL) is mandatory — P2P social channels (1/6/11)
     * must live there per spec, so discovery / listen / GO Neg traffic
     * is always on 2.4 GHz. Class 115 (5 GHz UNII-1, 36/40/44/48) is
     * added so a peer that prefers 5 GHz can pick a 5 GHz op_channel
     * during group formation; we then switch the radio to that freq for
     * data. UNII-2/2e/3 channels need DFS or country-specific allow-lists
     * we don't yet implement, so they're omitted for now.
     */
    memset(out, 0, sizeof(*out));
    out->reg_classes = 2;

    /* 2.4 GHz: op class 81, channels 1..11 */
    out->reg_class[0].reg_class = 81;
    out->reg_class[0].channels  = 11;
    for (size_t i = 0; i < 11; i++) {
        out->reg_class[0].channel[i] = (u8)(i + 1);
    }

    /* 5 GHz UNII-1: op class 115, channels 36/40/44/48 (no DFS) */
    out->reg_class[1].reg_class = 115;
    out->reg_class[1].channels  = 4;
    out->reg_class[1].channel[0] = 36;
    out->reg_class[1].channel[1] = 40;
    out->reg_class[1].channel[2] = 44;
    out->reg_class[1].channel[3] = 48;
}

/* ------------------------------ public ------------------------------ */

int qcom_p2p_enable(const struct qcom_p2p_params *params)
{
    struct p2p_config p2p;
    qapi_WLAN_P2P_Set_Config_t fw_cfg;

    if (params == NULL) {
        return -1;
    }
    if (s_ctx.handle != NULL) {
        return 0;
    }

    memcpy(&s_ctx.app_cfg, params, sizeof(s_ctx.app_cfg));
    s_ctx.app_cfg.pbc_auto_auth = true;

    qapi_WLAN_Set_Param(QCOM_DEV_STA_ID,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_SSID,
                        (void *)P2P_WILDCARD_SSID, P2P_WILDCARD_SSID_LEN,
                        false);

    /* Populate the WPS device-data the glue passes to wps_build_probe_req_ie
     * during every scan. wpas_p2p_scan reads these from
     * wpa_s->wps->dev / wpa_s->wps->uuid; without a supplicant we keep
     * an equivalent set of fields on the ctx. Strings stay valid for
     * the session — device_name is in the ctx BSS buffer, the
     * manufacturer/model/serial strings are file-scope statics.
     */
    memset(&s_ctx.wps, 0, sizeof(s_ctx.wps));
    memcpy(s_ctx.wps.data.mac_addr, params->dev_addr, ETH_ALEN);
    memcpy(s_ctx.wps.device_name, params->device_name,
           sizeof(s_ctx.wps.device_name) - 1);
    s_ctx.wps.data.device_name   = s_ctx.wps.device_name;
    s_ctx.wps.data.manufacturer  = g_wps_manufacturer;
    s_ctx.wps.data.model_name    = g_wps_model_name;
    s_ctx.wps.data.model_number  = g_wps_model_number;
    s_ctx.wps.data.serial_number = g_wps_serial_number;
    memcpy(s_ctx.wps.data.pri_dev_type, params->pri_dev_type, WPS_DEV_TYPE_LEN);
    s_ctx.wps.data.config_methods = params->config_methods;
    s_ctx.wps.data.rf_bands       = WPS_RF_24GHZ;
    /* p2p flag is flipped on/off at scan time inside qcom_p2p_scan. */

    if (uuid_random(s_ctx.wps.uuid) < 0) {
        /* Non-fatal — uuid_random falls back to internal entropy
         * sources; if that fails we leave the UUID zeroed which still
         * lets WPS Probe Req IE build (the spec allows nil UUID).
         */
        LOG_WRN("uuid_random failed; using nil UUID");
        memset(s_ctx.wps.uuid, 0, sizeof(s_ctx.wps.uuid));
    }

    memset(&p2p, 0, sizeof(p2p));
    p2p.cb_ctx          = &s_ctx;
    p2p.debug_print     = qcom_p2p_debug_print;
    p2p.p2p_scan        = qcom_p2p_scan;
    p2p.send_action     = qcom_send_action;
    p2p.send_action_done = qcom_send_action_done;
    p2p.go_neg_completed = qcom_go_neg_completed;
    p2p.go_neg_req_rx   = qcom_go_neg_req_rx;
    p2p.dev_found       = qcom_dev_found;
    p2p.dev_lost        = qcom_dev_lost;
    p2p.find_stopped    = qcom_find_stopped;
    p2p.start_listen    = qcom_start_listen;
    p2p.stop_listen     = qcom_stop_listen;
    p2p.send_probe_resp = qcom_send_probe_resp;
    p2p.sd_request      = qcom_sd_request;
    p2p.sd_response     = qcom_sd_response;
    p2p.prov_disc_req   = qcom_prov_disc_req;
    p2p.prov_disc_resp  = qcom_prov_disc_resp;
    p2p.prov_disc_fail  = qcom_prov_disc_fail;
    p2p.invitation_process  = qcom_invitation_process;
    p2p.invitation_received = qcom_invitation_received;
    p2p.invitation_result   = qcom_invitation_result;
    p2p.get_noa             = qcom_get_noa;
    p2p.go_connected        = qcom_go_connected;
    p2p.presence_resp       = qcom_presence_resp;
    p2p.is_concurrent_session_active = qcom_is_concurrent_session_active;
    p2p.is_p2p_in_progress  = qcom_p2p_in_progress;
    p2p.get_persistent_group = qcom_get_persistent_group;
    p2p.get_go_info         = qcom_get_go_info;
    p2p.remove_stale_groups = qcom_remove_stale_groups;
    p2p.p2ps_prov_complete  = qcom_p2ps_prov_complete;
    p2p.prov_disc_resp_cb   = qcom_prov_disc_resp_cb;
    p2p.get_pref_freq_list  = qcom_p2p_get_pref_freq_list;

    memcpy(p2p.dev_addr, params->dev_addr, ETH_ALEN);
    p2p.dev_name        = (char *)params->device_name;
    memcpy(p2p.pri_dev_type, params->pri_dev_type, WPS_DEV_TYPE_LEN);
    memcpy(p2p.country, params->country, 3);
    p2p.config_methods  = params->config_methods;
    p2p.reg_class       = params->listen_reg_class;
    p2p.channel         = params->listen_channel;
    p2p.channel_forced  = 1;
    p2p.op_reg_class    = params->op_reg_class;
    p2p.op_channel      = params->op_channel;
    p2p.cfg_op_channel  = 1;
    p2p.max_peers       = 100;
    p2p.max_listen      = 5000;
    p2p.passphrase_len  = 8;
    p2p.p2p_6ghz_disable = params->p2p_6ghz_disable;

    qcom_p2p_setup_channels(&p2p.channels);
    qcom_p2p_setup_channels(&p2p.cli_channels);

    s_ctx.handle = p2p_init(&p2p);
    if (s_ctx.handle == NULL) {
        LOG_ERR("p2p_init failed");
        return -1;
    }

    /* Set the P2P Interface Address advertised in GO Neg / Provision
     * Discovery frames.  Without this, hostap sends 00:00:00:00:00:00 as
     * intended_addr; the GO records a bogus address at GO-Neg time and
     * then treats the actual STA MAC as a *different* device when we
     * associate for WPS provisioning — which the GO's hostapd counts as a
     * second PBC enrollee and rejects with M2D config_error=12.
     *
     * On this build we don't have a separate P2P netdev, so the STA MAC
     * doubles as the P2P Interface Address.  It is the same value that
     * ends up in Assoc Req's SA / M1's MAC Address attribute, so the GO
     * sees a single consistent address across all frames. */
    p2p_set_intended_addr(s_ctx.handle, params->dev_addr);

    /* Log the device address actually used. Android Wi-Fi Direct
     * (and the Wi-Fi Direct spec generally) requires the P2P Device
     * Address to be a locally-administered MAC (bit 1 of the first
     * octet set, i.e. first byte & 0x02 != 0). If this address is a
     * "regular" globally-unique vendor MAC the peer may reject the
     * device or refuse to display it.
     */
    LOG_INF("P2P dev_addr=%02x:%02x:%02x:%02x:%02x:%02x (locally-admin=%d)",
            params->dev_addr[0], params->dev_addr[1], params->dev_addr[2],
            params->dev_addr[3], params->dev_addr[4], params->dev_addr[5],
            (params->dev_addr[0] & 0x02) ? 1 : 0);

    /* Also push minimal config to firmware so the FW-side WMI handler has
     * something on file should it need it for future extensions.
     */
    memset(&fw_cfg, 0, sizeof(fw_cfg));
    fw_cfg.go_intent      = 0;
    fw_cfg.reg_class      = params->listen_reg_class;
    fw_cfg.listen_channel = params->listen_channel;
    fw_cfg.op_reg_class   = params->op_reg_class;
    fw_cfg.op_channel     = params->op_channel;
    fw_cfg.node_age_to    = 60;
    fw_cfg.max_node_count = 5;
    (void)qapi_WLAN_P2P_Set_Config(0, &fw_cfg);

    wlan_drv_roaming_disable();

    LOG_INF("P2P enabled, listen=%d op=%d", params->listen_channel, params->op_channel);
    return 0;
}

int qcom_p2p_disable(void)
{
    wlan_drv_roaming_enable();

    if (s_ctx.handle == NULL) {
        return 0;   /* idempotent: disabling a never-enabled stack is a no-op */
    }

    /* Tear down hostap p2p state under the glue lock. p2p_deinit
     * internally calls p2p_flush / p2p_stop_find, which cancel all
     * eloop timeouts owned by the p2p module. Keep s_ctx.handle valid
     * during this window — WMI poster paths (qcom_p2p_feed_bss etc.)
     * null-check the handle, so we want NULL to become visible only
     * after the eloop thread is joined. */
    qcom_hostap_lock();
    p2p_deinit(s_ctx.handle);
    qcom_hostap_unlock();

    s_ctx.handle = NULL;
    memset(&s_ctx.app_cfg, 0, sizeof(s_ctx.app_cfg));
    memset(&s_ctx.wps,     0, sizeof(s_ctx.wps));
    memset(s_ctx.auth_invite_peer, 0, ETH_ALEN);
    memset(&s_ctx.last_tx, 0, sizeof(s_ctx.last_tx));

    /* Tell firmware to stop any P2P-driven scan engine work. The fw side
     * keys off g_p2p_cfg_valid (set by qcom_p2p_enable -> p2p_fw_set_config);
     * STOP_FIND is the simplest way to cause it to drop active scans.
     * We don't have a dedicated "P2P_DISABLE" WMI command — the fw
     * stays in P2P-aware mode until the next set_config or reboot. */
    (void)qapi_WLAN_P2P_Stop_Find(0);

    LOG_INF("P2P disabled");
    return 0;
}

int qcom_p2p_apply_runtime_cfg(const struct qcom_p2p_params *params)
{
    if (params == NULL || s_ctx.handle == NULL) {
        return -1;
    }

    qcom_hostap_lock();

    /* Validate every change against hostap first. All setters must
     * succeed before we touch glue-local state, otherwise a failure
     * mid-way would leave hostap and our snapshot diverged. Setters
     * return 0 on success, -1 on rejection (e.g. unsupported channel
     * for the given regulatory class). p2p_set_country and
     * p2p_set_pri_dev_type currently always succeed in hostap, but
     * treat them uniformly. p2p_set_config_methods has void return —
     * no validation, always succeeds. */
    int rc = p2p_set_dev_name(s_ctx.handle, params->device_name);
    if (rc) { LOG_ERR("p2p_set_dev_name failed (%d)", rc); goto out; }

    rc = p2p_set_country(s_ctx.handle, params->country);
    if (rc) { LOG_ERR("p2p_set_country failed (%d)", rc); goto out; }

    rc = p2p_set_pri_dev_type(s_ctx.handle, params->pri_dev_type);
    if (rc) { LOG_ERR("p2p_set_pri_dev_type failed (%d)", rc); goto out; }

    rc = p2p_set_listen_channel(s_ctx.handle,
                                params->listen_reg_class,
                                params->listen_channel,
                                1 /* forced */);
    if (rc) { LOG_ERR("p2p_set_listen_channel failed (%d)", rc); goto out; }

    rc = p2p_set_oper_channel(s_ctx.handle,
                              params->op_reg_class,
                              params->op_channel,
                              1 /* cfg_op_channel */);
    if (rc) { LOG_ERR("p2p_set_oper_channel failed (%d)", rc); goto out; }

    p2p_set_config_methods(s_ctx.handle, params->config_methods);

    /* All hostap setters succeeded — commit to ctx snapshots so
     * subsequent qcom_p2p_scan rebuilds the WPS Probe Req IE with the
     * new values. The device_name buffer inside ctx.wps is reused;
     * just refresh its contents. */
    memcpy(s_ctx.wps.device_name, params->device_name,
           sizeof(s_ctx.wps.device_name) - 1);
    s_ctx.wps.device_name[sizeof(s_ctx.wps.device_name) - 1] = '\0';
    memcpy(s_ctx.wps.data.pri_dev_type, params->pri_dev_type, WPS_DEV_TYPE_LEN);
    s_ctx.wps.data.config_methods = params->config_methods;
    {
        /* apply_cfg's params snapshot doesn't carry the live
         * pbc_auto_auth value (the shell's own copy always leaves it
         * default-initialized to false) — preserve the current setting
         * across this memcpy instead of clobbering it. */
        bool cur_pbc_auto_auth = s_ctx.app_cfg.pbc_auto_auth;
        memcpy(&s_ctx.app_cfg, params, sizeof(s_ctx.app_cfg));
        s_ctx.app_cfg.pbc_auto_auth = cur_pbc_auto_auth;
    }

    LOG_INF("P2P runtime cfg applied (listen=%d op=%d name=%s)",
            params->listen_channel, params->op_channel, params->device_name);

out:
    qcom_hostap_unlock();
    /* Note: we deliberately do NOT push qapi_WLAN_P2P_Set_Config here.
     * (1) It would deadlock — qapi_* blocks waiting on WMI ack and the
     *     ack handler may need the eloop, which needs this lock.
     * (2) listen_channel / op_channel are passed inline by every
     *     find / listen WMI command (qcom_p2p_scan / qcom_start_listen),
     *     so the firmware always sees the up-to-date values when it
     *     actually matters. set_config at enable time is enough. */
    return rc ? -1 : 0;
}

int qcom_p2p_apply_disc_int(int min_disc_int, int max_disc_int, int max_disc_tu)
{
    if (s_ctx.handle == NULL) {
        return -1;
    }
    qcom_hostap_lock();
    int rc = p2p_set_disc_int(s_ctx.handle, min_disc_int, max_disc_int,
                              max_disc_tu);
    qcom_hostap_unlock();
    if (rc) {
        LOG_ERR("p2p_set_disc_int(%d,%d,%d) failed", min_disc_int,
                max_disc_int, max_disc_tu);
        return -1;
    }
    LOG_INF("P2P disc_int set: min=%d max=%d max_tu=%d (100 TU units)",
            min_disc_int, max_disc_int, max_disc_tu);
    return 0;
}

int qcom_p2p_set_pbc_auto_auth(bool enable)
{
    if (s_ctx.handle == NULL) {
        return -1;
    }
    s_ctx.app_cfg.pbc_auto_auth = enable;
    LOG_INF("P2P pbc_auto_auth set: %s", enable ? "on" : "off");
    return 0;
}

int qcom_p2p_find_start(unsigned int timeout)
{
    int rc;

    if (s_ctx.handle == NULL) {
        return -1;
    }
    qcom_hostap_lock();
    rc = p2p_find(s_ctx.handle, timeout, P2P_FIND_START_WITH_FULL,
                  0, NULL, NULL, 0, 0, NULL, 0, false);
    qcom_hostap_unlock();
    return rc;
}

int qcom_p2p_connect(const uint8_t peer_mac[QCOM_P2P_MAC_LEN],
                     enum qcom_p2p_wps_method wps_method,
                     int go_intent, int persistent, int auth)
{
    enum p2p_wps_method m;
    int rc;

    if (s_ctx.handle == NULL) {
        LOG_ERR("p2p_connect: P2P not enabled");
        return -1;
    }
    if (peer_mac == NULL) {
        return -1;
    }
    if (go_intent < 0 || go_intent > 15) {
        LOG_ERR("p2p_connect: go_intent %d out of range", go_intent);
        return -1;
    }

    qcom_hostap_lock();
    if (p2p_get_peer_info(s_ctx.handle, peer_mac, 0) == NULL) {
        qcom_hostap_unlock();
        LOG_ERR("p2p_connect: peer not in table — run 'qwifi p2p find' first");
        return -1;
    }

    /* hostap's p2p_connect()/p2p_authorize() stop the *host* p2p FSM
     * internally (p2p_stop_find(p2p) when state != P2P_IDLE), but they
     * have no reach into fw. Without this, fw keeps running whatever
     * find/scan it was doing — including the wildcard "DIRECT-" PBC
     * Probe Request appie — for the entire GO Negotiation exchange that
     * follows, not just up to qcom_go_neg_completed(). The peer's GO can
     * see that stray PBC probe mid-negotiation and reject the later WPS
     * M2 with config_error=12 (PBC overlap), so stop fw's find *before*
     * GO Neg starts, not only after it completes. */
    (void)qapi_WLAN_P2P_Stop_Find(0);

    /* Map qcom enum to hostap enum (1:1 by construction). */
    switch (wps_method) {
    case QCOM_P2P_WPS_PIN_DISPLAY: m = WPS_PIN_DISPLAY; break;
    case QCOM_P2P_WPS_PIN_KEYPAD:  m = WPS_PIN_KEYPAD;  break;
    case QCOM_P2P_WPS_PBC:         m = WPS_PBC;         break;
    case QCOM_P2P_WPS_NFC:         m = WPS_NFC;         break;
    case QCOM_P2P_WPS_P2PS:        m = WPS_P2PS;        break;
    default:                       m = WPS_NOT_READY;   break;
    }

    /* own_interface_addr: re-use the device address. A real
     * implementation should derive a separate group-iface address
     * (see qcom_p2p_mac_setup in fermion's path), but for skeleton
     * purposes the device addr is enough for hostap to start GO neg.
     *
     * auth=1 path mirrors wpa_cli "p2p_connect ... auth": stage the
     * wps_method/go_intent in the peer's dev struct and wait for the
     * peer to retry GO Neg Req. Use this when responding to a
     * P2P-GO-NEG-REQUEST event. p2p_authorize takes the same args as
     * p2p_connect minus pd_before_go_neg.
     */
    if (auth) {
        rc = p2p_authorize(s_ctx.handle, peer_mac, m, go_intent,
                           s_ctx.app_cfg.dev_addr,
                           0,                  /* force_freq */
                           persistent,
                           NULL, 0,            /* force_ssid */
                           0,                  /* pref_freq */
                           0);                 /* oob_pw_id */
    } else {
        rc = p2p_connect(s_ctx.handle, peer_mac, m, go_intent,
                         s_ctx.app_cfg.dev_addr,
                         0,                  /* force_freq */
                         persistent,
                         NULL, 0,            /* force_ssid */
                         0,                  /* pd_before_go_neg */
                         0,                  /* pref_freq */
                         0);                 /* oob_pw_id */
    }
    qcom_hostap_unlock();
    if (rc < 0) {
        LOG_ERR("hostap %s() rejected (rc=%d). Likely a stub callback"
                " (send_action / start_listen) returned -1.",
                auth ? "p2p_authorize" : "p2p_connect", rc);
        return -1;
    }
    LOG_INF("p2p_%s issued (peer=%02x:%02x:%02x:%02x:%02x:%02x"
            " method=%d go_intent=%d)%s",
            auth ? "authorize" : "connect",
            peer_mac[0], peer_mac[1], peer_mac[2],
            peer_mac[3], peer_mac[4], peer_mac[5], m, go_intent,
            auth ? " — waiting for peer to retry GO Neg" : "");
    return 0;
}

int qcom_p2p_find_stop(void)
{
    if (s_ctx.handle == NULL) {
        return -1;
    }
    /* hostap p2p_stop_find clears its FSM (cancels eloop timeout, drops
     * pending listen, transitions out of P2P_SEARCH). It also invokes
     * cfg->p2p_scan_done indirectly — but we additionally fire the WMI
     * stop_find so the firmware aborts any in-flight scan immediately
     * rather than letting it finish naturally.
     */
    qcom_hostap_lock();
    p2p_stop_find(s_ctx.handle);
    qcom_hostap_unlock();
    (void)qapi_WLAN_P2P_Stop_Find(0);
    return 0;
}

int qcom_p2p_listen_start(unsigned int timeout_sec)
{
    int rc;

    if (s_ctx.handle == NULL) {
        return -1;
    }
    /* hostap expects timeout in seconds; 0 lets it pick the default
     * (5 s, capped by p2p->cfg->max_listen). */
    qcom_hostap_lock();
    rc = p2p_listen(s_ctx.handle, timeout_sec);
    qcom_hostap_unlock();
    if (rc < 0) {
        LOG_ERR("p2p_listen rejected (rc=%d) — already listening?", rc);
        return -1;
    }
    return 0;
}

int qcom_p2p_cancel(void)
{
    const u8 *peer;

    if (s_ctx.handle == NULL) {
        return -1;
    }
    /* Mirrors wpas_p2p_cancel: drop the GO-neg peer authorization (if
     * any) so the peer can't continue, then stop find. We don't
     * implement group-formation rollback — at this stage we don't
     * fully bring up groups anyway. */
    qcom_hostap_lock();
    peer = p2p_get_go_neg_peer(s_ctx.handle);
    if (peer != NULL) {
        LOG_INF("p2p_cancel: unauthorize pending GO Neg peer "
                "%02x:%02x:%02x:%02x:%02x:%02x",
                peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
        (void)p2p_unauthorize(s_ctx.handle, peer);
    }
    p2p_stop_find(s_ctx.handle);
    qcom_hostap_unlock();
    (void)qapi_WLAN_P2P_Stop_Find(0);
    return 0;
}

int qcom_p2p_flush(void)
{
    if (s_ctx.handle == NULL) {
        return -1;
    }
    /* p2p_flush wipes the entire device table inside hostap. Subsequent
     * `qwifi p2p peers` shows nothing until a new find sees them. */
    qcom_hostap_lock();
    p2p_flush(s_ctx.handle);
    qcom_hostap_unlock();
    return 0;
}

int qcom_p2p_reject(const uint8_t peer_mac[QCOM_P2P_MAC_LEN])
{
    int rc;

    if (s_ctx.handle == NULL || peer_mac == NULL) {
        return -1;
    }
    qcom_hostap_lock();
    rc = p2p_reject(s_ctx.handle, peer_mac);
    qcom_hostap_unlock();
    if (rc < 0) {
        LOG_ERR("p2p_reject rejected (rc=%d) — peer not in table?", rc);
        return -1;
    }
    return 0;
}

int qcom_p2p_authorize_invite(const uint8_t peer_mac[QCOM_P2P_MAC_LEN])
{
    /* Read by qcom_invitation_process on the eloop thread; the shell
     * thread mutates here. The 6-byte MAC fits in a single store on
     * Cortex-M4 only piecewise — take the lock to publish atomically.
     * is_zero_ether_addr / ether_addr_equal are pure reads, no lock
     * needed at the read side beyond what hostap already holds. */
    qcom_hostap_lock();
    if (peer_mac == NULL) {
        memset(s_ctx.auth_invite_peer, 0, ETH_ALEN);
        LOG_INF("P2P invitation pre-auth cleared");
    } else {
        memcpy(s_ctx.auth_invite_peer, peer_mac, ETH_ALEN);
        LOG_INF("P2P invitation pre-auth set: %02x:%02x:%02x:%02x:%02x:%02x",
                peer_mac[0], peer_mac[1], peer_mac[2],
                peer_mac[3], peer_mac[4], peer_mac[5]);
    }
    qcom_hostap_unlock();
    return 0;
}

int qcom_p2p_invite(const uint8_t peer_mac[QCOM_P2P_MAC_LEN],
                    enum qcom_p2p_invite_role role,
                    const uint8_t *bssid,
                    const uint8_t *ssid, size_t ssid_len,
                    unsigned int freq,
                    int persistent_group)
{
    enum p2p_invite_role hr;
    int rc;

    if (s_ctx.handle == NULL || peer_mac == NULL ||
        ssid == NULL || ssid_len == 0) {
        return -1;
    }
    switch (role) {
    case QCOM_P2P_INVITE_ROLE_GO:        hr = P2P_INVITE_ROLE_GO;        break;
    case QCOM_P2P_INVITE_ROLE_ACTIVE_GO: hr = P2P_INVITE_ROLE_ACTIVE_GO; break;
    case QCOM_P2P_INVITE_ROLE_CLIENT:    hr = P2P_INVITE_ROLE_CLIENT;    break;
    default:                             return -1;
    }

    qcom_hostap_lock();
    rc = p2p_invite(s_ctx.handle, peer_mac, hr,
                    bssid,
                    ssid, ssid_len,
                    freq,
                    NULL,             /* go_dev_addr (forced GO addr) */
                    persistent_group,
                    0,                /* pref_freq */
                    -1);              /* dev_pw_id (NFC OOB; -1 = none) */
    qcom_hostap_unlock();
    if (rc < 0) {
        LOG_ERR("p2p_invite rejected (rc=%d) — peer not in table?", rc);
        return -1;
    }
    LOG_INF("P2P-INVITE issued (peer=%02x:%02x:%02x:%02x:%02x:%02x"
            " role=%d freq=%u ssid=\"%.*s\")",
            peer_mac[0], peer_mac[1], peer_mac[2],
            peer_mac[3], peer_mac[4], peer_mac[5],
            role, freq, (int)ssid_len, (const char *)ssid);
    return 0;
}

/* ----------------------- peer-list inspection ----------------------- */

int qcom_p2p_peers_dump(qcom_p2p_print_cb cb, void *cb_ctx)
{
    const struct p2p_peer_info *info;
    const u8 *prev_addr = NULL;
    int count = 0;

    if (s_ctx.handle == NULL || cb == NULL) {
        return -1;
    }

    /* Walk the hostap-internal device list. p2p_get_peer_found takes
     * (NULL, 1) for the first entry, then (prev_addr, 1) for the
     * subsequent ones — same idiom used by wpa_cli "p2p_peers".
     */
    qcom_hostap_lock();
    while ((info = p2p_get_peer_found(s_ctx.handle, prev_addr, 1)) != NULL) {
        cb(cb_ctx, "%02x:%02x:%02x:%02x:%02x:%02x %s",
           info->p2p_device_addr[0], info->p2p_device_addr[1],
           info->p2p_device_addr[2], info->p2p_device_addr[3],
           info->p2p_device_addr[4], info->p2p_device_addr[5],
           info->device_name[0] ? info->device_name : "(no name)");
        prev_addr = info->p2p_device_addr;
        count++;
    }
    qcom_hostap_unlock();
    return count;
}

int qcom_p2p_peer_dump(const uint8_t mac[QCOM_P2P_MAC_LEN],
                       qcom_p2p_print_cb cb, void *cb_ctx)
{
    const struct p2p_peer_info *info;

    if (s_ctx.handle == NULL || cb == NULL || mac == NULL) {
        return -1;
    }

    qcom_hostap_lock();
    info = p2p_get_peer_info(s_ctx.handle, mac, 0);
    if (info == NULL) {
        qcom_hostap_unlock();
        return -1;
    }

    cb(cb_ctx, "address=%02x:%02x:%02x:%02x:%02x:%02x",
       info->p2p_device_addr[0], info->p2p_device_addr[1],
       info->p2p_device_addr[2], info->p2p_device_addr[3],
       info->p2p_device_addr[4], info->p2p_device_addr[5]);
    cb(cb_ctx, "device_name=%s",
       info->device_name[0] ? info->device_name : "(none)");
    /* WPS device attributes are optional in P2P probe responses —
     * Android peers in particular only fill device_name during
     * discovery and leave manufacturer/model fields blank until WPS
     * provisioning. Skip empty lines for a cleaner dump. */
    if (info->manufacturer[0]) {
        cb(cb_ctx, "manufacturer=%s", info->manufacturer);
    }
    if (info->model_name[0]) {
        cb(cb_ctx, "model_name=%s",   info->model_name);
    }
    if (info->model_number[0]) {
        cb(cb_ctx, "model_number=%s", info->model_number);
    }
    if (info->serial_number[0]) {
        cb(cb_ctx, "serial_number=%s", info->serial_number);
    }
    cb(cb_ctx, "config_methods=0x%x", info->config_methods);
    cb(cb_ctx, "dev_capab=0x%x",      info->dev_capab);
    cb(cb_ctx, "group_capab=0x%x",    info->group_capab);
    cb(cb_ctx, "level=%d",            info->level);
    qcom_hostap_unlock();
    return 0;
}

/* ---------------- WMI -> hostap event entry points ---------------- */

/*
 * The qcom_p2p_* entry points below are strong overrides of the weak
 * fallbacks in wmi_api.c. They run on the WMI dispatch task — NOT on the
 * hostap eloop thread — so they must not call hostap APIs directly. Each
 * one allocates a struct qcom_he_msg-derived message, copies the payload,
 * and submits via qcom_hostap_post(). The eloop thread later runs the
 * matching handle_*() callback under the glue lock and frees the message.
 *
 * USD reuse: NAN DE will follow the same pattern with its own message
 * structs and handlers; the generic eloop infrastructure stays untouched.
 */

/* --- p2p BSS / scan-result --- */
struct he_msg_p2p_bss {
    struct qcom_he_msg msg;
    uint8_t  bssid[QCOM_P2P_MAC_LEN];
    int      freq;
    int      rssi_dbm;
    size_t   ies_len;
    uint8_t *ies; /* heap, freed by handler */
};

static void handle_p2p_bss(struct qcom_he_msg *m)
{
    struct he_msg_p2p_bss *e = CONTAINER_OF(m, struct he_msg_p2p_bss, msg);
    struct os_reltime rx_time;

    if (s_ctx.handle != NULL) {
        int freq = e->freq;
        if (freq == 0) {
            /* fw didn't fill chan_freq (discovery.c chan_freq bug when DS
             * Parameter Set IE is absent).  Fall back to our own listen
             * channel: P2P social channels (1/6/11) are spec-required, so
             * the peer is on the same channel we are. */
            freq = 2407 + s_ctx.app_cfg.listen_channel * 5;
        }
        os_get_reltime(&rx_time);
        (void)p2p_scan_res_handler(s_ctx.handle, e->bssid, freq, &rx_time,
                                   e->rssi_dbm, e->ies, e->ies_len);
    }
    if (e->ies != NULL) {
        k_free(e->ies);
    }
    k_free(e);
}

int qcom_p2p_feed_bss(const uint8_t bssid[QCOM_P2P_MAC_LEN], int freq,
                      int rssi_dbm, const u8 *ies, size_t ies_len)
{
    struct he_msg_p2p_bss *e;

    if (s_ctx.handle == NULL) {
        return -1;
    }
    e = k_malloc(sizeof(*e));
    if (e == NULL) {
        return -ENOMEM;
    }
    memset(e, 0, sizeof(*e));
    e->msg.handle = handle_p2p_bss;
    memcpy(e->bssid, bssid, QCOM_P2P_MAC_LEN);
    e->freq     = freq;
    e->rssi_dbm = rssi_dbm;
    e->ies_len  = ies_len;
    if (ies != NULL && ies_len > 0) {
        e->ies = k_malloc(ies_len);
        if (e->ies == NULL) {
            k_free(e);
            return -ENOMEM;
        }
        memcpy(e->ies, ies, ies_len);
    }
    if (qcom_hostap_post(&e->msg) != 0) {
        if (e->ies != NULL) {
            k_free(e->ies);
        }
        k_free(e);
        return -EAGAIN;
    }
    return 0;
}

/* --- p2p scan done --- */
static void handle_p2p_scan_done(struct qcom_he_msg *m)
{
    if (s_ctx.handle != NULL) {
        p2p_scan_res_handled(s_ctx.handle, 0);
    }
    k_free(m);
}

void qcom_p2p_scan_done(void)
{
    struct qcom_he_msg *m;

    if (s_ctx.handle == NULL) {
        return;
    }
    m = k_malloc(sizeof(*m));
    if (m == NULL) {
        return;
    }
    memset(m, 0, sizeof(*m));
    m->handle = handle_p2p_scan_done;
    if (qcom_hostap_post(m) != 0) {
        k_free(m);
    }
}

/* --- p2p listen-started (driver entered Listen state) --- */
struct he_msg_p2p_listen_started {
    struct qcom_he_msg msg;
    unsigned int freq;
    unsigned int duration_ms;
};

static void handle_p2p_listen_started(struct qcom_he_msg *m)
{
    struct he_msg_p2p_listen_started *e =
        CONTAINER_OF(m, struct he_msg_p2p_listen_started, msg);

    /* Confirm to hostap that the driver entered Listen state. Without this
     * hostap keeps pending_listen_freq set and the SEARCH/LISTEN state
     * machine never arms its timeout, so subsequent SCAN_DONE events spin
     * without a real listen-end transition. */
    if (s_ctx.handle != NULL) {
        p2p_listen_cb(s_ctx.handle, e->freq, e->duration_ms);
    }
    k_free(e);
}

static int qcom_hostap_post_p2p_listen_started(unsigned int freq,
                                               unsigned int duration_ms)
{
    struct he_msg_p2p_listen_started *e;

    e = k_malloc(sizeof(*e));
    if (e == NULL) {
        return -ENOMEM;
    }
    memset(e, 0, sizeof(*e));
    e->msg.handle    = handle_p2p_listen_started;
    e->freq          = freq;
    e->duration_ms   = duration_ms;
    if (qcom_hostap_post(&e->msg) != 0) {
        k_free(e);
        return -EAGAIN;
    }
    return 0;
}

/* --- p2p listen end --- */
struct he_msg_p2p_listen_end {
    struct qcom_he_msg msg;
    unsigned int freq;
};

static void handle_p2p_listen_end(struct qcom_he_msg *m)
{
    struct he_msg_p2p_listen_end *e =
        CONTAINER_OF(m, struct he_msg_p2p_listen_end, msg);

    /* Don't log — these fire every listen window (very chatty). */
    if (s_ctx.handle != NULL) {
        (void)p2p_listen_end(s_ctx.handle, e->freq);
    }
    k_free(e);
}

void qcom_p2p_listen_end(unsigned int freq)
{
    struct he_msg_p2p_listen_end *e;

    if (s_ctx.handle == NULL) {
        return;
    }
    e = k_malloc(sizeof(*e));
    if (e == NULL) {
        return;
    }
    memset(e, 0, sizeof(*e));
    e->msg.handle = handle_p2p_listen_end;
    e->freq       = freq;
    if (qcom_hostap_post(&e->msg) != 0) {
        k_free(e);
    }
}

/* --- p2p RX action frame ---
 *
 * Forwarded to the eloop dispatcher so hostap p2p_rx_action runs on the
 * eloop thread with the glue lock held — same pattern as scan_done /
 * listen_end. Frame layout:
 *   [0-1]   frame control (subtype = 0xd0 = ACTION)
 *   [2-3]   duration
 *   [4-9]   addr1 = DA
 *   [10-15] addr2 = SA
 *   [16-21] addr3 = BSSID
 *   [22-23] seq
 *   [24]    category
 *   [25+]   action body (data passed to hostap)
 */
struct he_msg_p2p_rx_action {
    struct qcom_he_msg msg;
    unsigned int freq;
    size_t       frame_len;
    uint8_t     *frame; /* heap, freed by handler */
};

static void handle_p2p_rx_action(struct qcom_he_msg *m)
{
    struct he_msg_p2p_rx_action *e =
        CONTAINER_OF(m, struct he_msg_p2p_rx_action, msg);

    if (s_ctx.handle != NULL && e->frame != NULL && e->frame_len >= 25) {
        const uint8_t *da     = &e->frame[4];
        const uint8_t *sa     = &e->frame[10];
        const uint8_t *bssid  = &e->frame[16];
        uint8_t        cat    = e->frame[24];
        const uint8_t *data   = &e->frame[25];
        size_t         data_len = e->frame_len - 25;
        p2p_rx_action(s_ctx.handle, da, sa, bssid, cat, data, data_len,
                      (int)e->freq);
    }
    if (e->frame != NULL) {
        k_free(e->frame);
    }
    k_free(e);
}

int qcom_p2p_rx_action(const uint8_t *frame, size_t frame_len,
                       unsigned int freq)
{
    struct he_msg_p2p_rx_action *e;

    /* 24-byte 802.11 mgmt header + 1B category — anything shorter isn't a
     * parseable action frame. */
    if (s_ctx.handle == NULL || frame == NULL || frame_len < 24 + 1) {
        return -1;
    }
    e = k_malloc(sizeof(*e));
    if (e == NULL) {
        return -ENOMEM;
    }
    memset(e, 0, sizeof(*e));
    e->msg.handle = handle_p2p_rx_action;
    e->freq       = freq;
    e->frame_len  = frame_len;
    e->frame      = k_malloc(frame_len);
    if (e->frame == NULL) {
        k_free(e);
        return -ENOMEM;
    }
    memcpy(e->frame, frame, frame_len);
    if (qcom_hostap_post(&e->msg) != 0) {
        k_free(e->frame);
        k_free(e);
        return -EAGAIN;
    }
    return 0;
}

/* --- p2p Action TX done ---
 *
 * Hostap p2p arms the GO-Neg-Request retransmission timer in
 * p2p_send_action_cb; without this notification it stays stuck in
 * P2P_PENDING_GO_NEG_REQUEST forever and never retries.
 */
struct he_msg_p2p_action_tx_done {
    struct qcom_he_msg msg;
    unsigned int freq;
    uint8_t      dst[QCOM_P2P_MAC_LEN];
    uint8_t      src[QCOM_P2P_MAC_LEN];
    uint8_t      bssid[QCOM_P2P_MAC_LEN];
    int          success; /* 1 = SUCCESS, 0 = NO_ACK/FAILED */
};

static void handle_p2p_action_tx_done(struct qcom_he_msg *m)
{
    struct he_msg_p2p_action_tx_done *e =
        CONTAINER_OF(m, struct he_msg_p2p_action_tx_done, msg);

    if (s_ctx.handle != NULL) {
        enum p2p_send_action_result res =
            e->success ? P2P_SEND_ACTION_SUCCESS : P2P_SEND_ACTION_NO_ACK;
        LOG_INF("P2P-ACTION-TX-DONE dst=%02x:%02x:%02x:%02x:%02x:%02x"
                " freq=%u success=%d",
                e->dst[0], e->dst[1], e->dst[2], e->dst[3],
                e->dst[4], e->dst[5], e->freq, e->success);
        p2p_send_action_cb(s_ctx.handle, e->freq, e->dst, e->src,
                           e->bssid, res);
    }
    k_free(e);
}

static int qcom_hostap_post_p2p_action_tx_done(unsigned int freq,
                                               const uint8_t *dst,
                                               const uint8_t *src,
                                               const uint8_t *bssid,
                                               int success)
{
    struct he_msg_p2p_action_tx_done *e;

    e = k_malloc(sizeof(*e));
    if (e == NULL) {
        return -ENOMEM;
    }
    memset(e, 0, sizeof(*e));
    e->msg.handle = handle_p2p_action_tx_done;
    e->freq       = freq;
    e->success    = success;
    memcpy(e->dst,   dst,   QCOM_P2P_MAC_LEN);
    memcpy(e->src,   src,   QCOM_P2P_MAC_LEN);
    memcpy(e->bssid, bssid, QCOM_P2P_MAC_LEN);
    if (qcom_hostap_post(&e->msg) != 0) {
        k_free(e);
        return -EAGAIN;
    }
    return 0;
}
