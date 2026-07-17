/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/shell/shell.h>
#include <string.h>

#include "qcom_wps_glue.h"
#include "qcom_hostap_eloop.h"
#include "qapi_wlan_base.h"
#include "qapi_wlan_param_group.h"   /* __QAPI_WLAN_PARAM_GROUP_WIRELESS_APP_IE */
#include "wlan_drv.h"                /* WMI_FRAME_PROBE_REQ, QCOM_DEV_STA_ID */
#include "wlan_qapi_helper.h"        /* wlan_clear_privacy */

/* hostap WPS + eloop + l2_packet layer */
#include "wps/wps.h"
#include "wps/wps_i.h"
#include "wps/wps_defs.h"
#include "wps/wps_attr_parse.h"
#include "eap_common/eap_wsc_common.h"
#include "utils/eloop.h"
#include "utils/common.h"   /* WPA_GET_BE16, WPA_PUT_BE16, WPA_PUT_BE32 */
#include "l2_packet/l2_packet.h"
#include "crypto/dh_group5.h"

LOG_MODULE_REGISTER(qcom_wps_glue, CONFIG_WIFI_LOG_LEVEL);

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */

extern void nt_dpm_set_eap_enterprise_hook(void (*fn)(const uint8_t *src_addr,
                                                       const uint8_t *eapol_data,
                                                       uint16_t eapol_len));

/*
 * Mirror of sta_config_t from prop/libwifiqcc730/dpm/inc/mlme_al.h.
 * Layout must stay in sync (all uint8_t — no padding).
 * sec_mode: NONE=0, WEP40=1, WEP104=2, AES=3, TKIP=4
 */
typedef struct {
    uint8_t bssid[6];
    uint8_t IsAP;
    uint8_t sta_mac_address[6];
    uint8_t sta_sig;
    uint8_t dpu_sig;
    uint8_t qos_sta;
    uint8_t sec_mode;
    uint8_t rmf;
    uint8_t ht;
} wps_sta_cfg_t;

extern int nt_dpm_add_sta(void *sta_config, uint8_t *staid, uint8_t hal_sta_idx);
extern int nt_dpm_delete_sta(uint8_t staid);

/* Internal functions defined later in this file */
static int qcom_wps_scan(const struct device *dev);
static void reset_scan_state(void);
static int  wps_rf_band_cb(void *ctx);

static void wps_event_cb(void *ctx, enum wps_event event,
                          union wps_event_data *data)
{
    ARG_UNUSED(ctx);
    switch (event) {
    case WPS_EV_M2D: {
        u16 cfg_err = data ? data->m2d.config_error : 0;
        const char *reason;
        switch (cfg_err) {
        case WPS_CFG_NO_ERROR:
            reason = "AP waiting for user confirmation";
            break;
        case WPS_CFG_MULTIPLE_PBC_DETECTED:
            reason = "PBC overlap detected by AP";
            break;
        case WPS_CFG_SETUP_LOCKED:
            reason = "AP setup locked (too many failures)";
            break;
        case WPS_CFG_DEVICE_BUSY:
            reason = "AP device busy";
            break;
        default:
            reason = "see config_error code";
            break;
        }
        LOG_WRN("wps_event_cb: M2D config_error=%u (%s)", cfg_err, reason);
        break;
    }
    case WPS_EV_FAIL: {
        u16 cfg_err = data ? data->fail.config_error : 0;
        int msg     = data ? data->fail.msg : 0;
        LOG_WRN("wps_event_cb: WPS_FAIL msg=%d config_error=%u", msg, cfg_err);
        break;
    }
    case WPS_EV_SUCCESS:
        LOG_INF("wps_event_cb: WPS_SUCCESS");
        break;
    case WPS_EV_PBC_OVERLAP:
        LOG_WRN("wps_event_cb: PBC overlap — multiple APs with WPS button active");
        break;
    default:
        LOG_DBG("wps_event_cb: event=%d", (int)event);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* WSC IE parse result                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    bool    valid;
    bool    pbc_active;       /* wps_is_selected_pbc_registrar() result */
    bool    has_uuid;
    uint8_t uuid[WPS_UUID_LEN];
} wsc_ie_info_t;

/*
 * Parse WSC IE payload:
 *   - pbc_active  : via wps_is_selected_pbc_registrar() (hostap)
 *   - uuid_e      : via wps_parse_msg() for UUID dedup
 *
 * safe_len truncation prevents partial-TLV parse failure when firmware
 * caps the IE at WMI_WPS_SCAN_IE_MAX_LEN (210 bytes).
 */
static wsc_ie_info_t parse_wsc_ie(const uint8_t *payload, uint8_t payload_len)
{
    wsc_ie_info_t info = {0};
    struct wpabuf buf;

    /* Find last complete TLV */
    uint16_t safe_len = 0;
    uint16_t pos = 0;
    while (pos + 4 <= payload_len) {
        uint16_t attr_len = WPA_GET_BE16(payload + pos + 2);
        uint16_t tlv_end  = pos + 4 + attr_len;
        if (tlv_end > payload_len)
            break;
        safe_len = tlv_end;
        pos = tlv_end;
    }
    if (safe_len == 0)
        return info;

    wpabuf_set(&buf, payload, safe_len);

    /* PBC active check — reuse hostap's wps_is_selected_pbc_registrar() */
    info.pbc_active = (wps_is_selected_pbc_registrar(&buf) == 1);

    /* UUID-E extraction for overlap dedup — requires full attr parse */
    struct wps_parse_attr attr;
    if (wps_parse_msg(&buf, &attr) == 0) {
        info.valid = true;
        if (attr.uuid_e) {
            info.has_uuid = true;
            memcpy(info.uuid, attr.uuid_e, WPS_UUID_LEN);
        }
    }

    return info;
}

/* ------------------------------------------------------------------ */
/* Module state                                                         */
/* ------------------------------------------------------------------ */

#define WPS_MAX_PBC_REGISTRAR  8

/*
 * g_wps — WPS enrollee session state for QCC730.
 *
 * Design mirrors wpa_supplicant's WPS-related fields, stripped to
 * enrollee-only needs:
 *
 *  wpa_supplicant field       g_wps equivalent       Notes
 *  ─────────────────────────  ─────────────────────  ──────────────────────
 *  struct wps_context *wps    wps_ctx                long-term WPS config
 *  (wps_data via wps_init())  wps_data               per-session protocol state
 *  wps_success                wps_success            M8 completed flag
 *  wps_run                    (not needed)           session counter, unused here
 *  supp_pbc_active            supp_pbc_active        PBC session in progress
 *  wps_overlap                wps_overlap            PBC overlap flag
 *  wps_scan_done              (implicit via scanning) scan round state
 *  struct wps_ap_info *wps_ap target_ap              selected PBC-active AP
 *  num_wps_ap / wps_ap_iter   pbc_uuid[] dedup table distinct registrar tracking
 *  wps_fragment_size          (not needed)           EAP fragmentation in FW
 *  wps_freq / known_wps_freq  target_ap.channel      target AP channel
 *  after_wps                  (not needed)           supplicant reconnect logic
 *
 * Fields not in wpa_supplicant but needed here:
 *  dev / device_id            Zephyr device + QAPI device ID
 *  cred_cb                    (removed)              wps_context.cred_cb handles this
 *  scanning                   scan round in progress flag
 *  pbc_uuid[] / counts        UUID dedup (wpa_supplicant uses wps_ap_info array)
 */
static struct {
    /* Device context */
    const struct device    *dev;        /* Zephyr device pointer */
    uint8_t                 device_id;  /* QAPI device ID */

    /* Protocol context (hostap) */
    struct wps_context     *wps_ctx;    /* long-term config (uuid, dev info, callbacks) */
    struct wps_data        *wps_data;   /* per-session M1-M8 state */

    /* Session status — mirrors wpa_supplicant */
    bool    supp_pbc_active;    /* PBC session started (wpa_supplicant::supp_pbc_active) */
    bool    wps_success;        /* M8 done, credentials received (wpa_supplicant::wps_success) */
    bool    wps_overlap;        /* PBC overlap detected (wpa_supplicant::wps_overlap) */

    /* Scan phase — per-round, reset before each new scan */
    bool    scanning;           /* scan round in progress */

    /* Target AP selected during scan (mirrors wps_ap_info) */
    struct qcom_wps_scan_result target_ap;
    bool                        target_found;

    /* UUID dedup table for PBC overlap (mirrors wps_ap_info array + iter) */
    uint8_t  pbc_uuid[WPS_MAX_PBC_REGISTRAR][WPS_UUID_LEN];
    uint8_t  pbc_uuid_count;       /* entries with valid UUID */
    uint8_t  pbc_no_uuid_count;    /* PBC-active APs without UUID (conservative) */

    /*
     * Connected AP BSSID — set in qcom_wps_connect(), used in
     * qcom_wps_assoc_event() to verify the assoc event is for our target.
     */
    uint8_t  ap_bssid[6];

    /* EAP TX path — ENC_NONE DPM STA entry for EAPOL frame routing */
    bool     eap_sta_added;
    uint8_t  eap_staid;
    struct l2_packet_data *l2;  /* persistent l2 handle for EAP-WSC session */

    /* Credentials received from M8 — applied after WPS session fully cancelled */
    bool                    cred_valid;
    struct wps_credential   cred;

    /* Optional scan filter — set by qcom_wps_start_pbc(), persists for the
     * full PBC session (all rescan rounds).  Cleared by qcom_wps_cancel()
     * via memset.  channel_count==0 and all-zero bssid means no filter. */
    qapi_WLAN_WPS_Scan_Params_t scan_params;

    /*
     * Timers via hostap eloop (callbacks run on eloop thread):
     *   pbc_walk_timer : 120 s — WSC spec 11.1 PBC walk time window
     *   session_timer  : 120 s — M1-M8 exchange deadline after assoc
     * No struct fields needed — eloop tracks timers by handler pointer.
     */
} g_wps;

/* Returns total distinct PBC registrars seen this round */
static uint8_t distinct_pbc_count(void)
{
    return g_wps.pbc_uuid_count + g_wps.pbc_no_uuid_count;
}

/* Reset per-round scan state — called before each new scan */
static void reset_scan_state(void)
{
    g_wps.scanning           = false;
    g_wps.wps_overlap        = false;
    g_wps.target_found       = false;
    g_wps.pbc_uuid_count     = 0;
    g_wps.pbc_no_uuid_count  = 0;
    memset(&g_wps.target_ap,  0, sizeof(g_wps.target_ap));
    memset(g_wps.pbc_uuid,    0, sizeof(g_wps.pbc_uuid));
}

/* ------------------------------------------------------------------ */
/* Timer callbacks (run on eloop thread)                                */
/* ------------------------------------------------------------------ */

static void pbc_walk_timer_fn(void *eloop_ctx, void *user_ctx)
{
    ARG_UNUSED(eloop_ctx);
    ARG_UNUSED(user_ctx);
    LOG_WRN("pbc_walk_timer: PBC walk time (%d s) expired", WPS_PBC_WALK_TIME);
    qcom_wps_cancel(g_wps.dev);
}

static void session_timer_fn(void *eloop_ctx, void *user_ctx)
{
    ARG_UNUSED(eloop_ctx);
    ARG_UNUSED(user_ctx);
    LOG_WRN("session_timer: M1-M8 session timeout (%d s) expired", WPS_PBC_WALK_TIME);
    qcom_wps_cancel(g_wps.dev);
}

/* rf_band_cb — return current RF band for the enrollee connection.
 * Called by wps_build_m1() to set the RF Bands attribute in M1.
 * WPS_RF_24GHZ=0x01, WPS_RF_50GHZ=0x02 (wps_defs.h)
 */
static int wps_rf_band_cb(void *ctx)
{
    ARG_UNUSED(ctx);
    if (g_wps.target_ap.channel > 0 && g_wps.target_ap.channel <= 14)
        return WPS_RF_24GHZ;
    if (g_wps.target_ap.channel >= 36)
        return WPS_RF_50GHZ;
    return WPS_RF_24GHZ | WPS_RF_50GHZ;  /* fallback: advertise both */
}

/*
 * Deferred callbacks — posted via qcom_hostap_post() from
 * qwifi_wps_scan_event() which runs under wlan_qapi_cxt_mutex.  Calling
 * qapi_WLAN_WPS_Scan() directly from that context would deadlock because
 * wmi_wps_scan() tries to acquire the same mutex.  qcom_hostap_post()
 * enqueues the message into g_he_fifo and wakes the eloop thread via
 * eventfd; the handler runs on the eloop thread after the mutex is released.
 *
 * Each message struct embeds qcom_he_msg as its first member (required by
 * qcom_hostap_post / CONTAINER_OF convention).  The handler frees the
 * allocation before returning.
 */

static void scan_stop_handle(struct qcom_he_msg *he)
{
    os_free(he);  /* free before calling API — no further use of msg */
    qapi_WLAN_WPS_Scan_Params_t stop = { .op = QAPI_WLAN_WPS_SCAN_STOP_E };
    qapi_WLAN_WPS_Scan(0, &stop);
}

static void rescan_handle(struct qcom_he_msg *he)
{
    os_free(he);
    reset_scan_state();
    qcom_wps_scan(g_wps.dev);
}

static void cancel_handle(struct qcom_he_msg *he)
{
    os_free(he);
    qcom_wps_cancel(g_wps.dev);
}

static void connect_handle(struct qcom_he_msg *he)
{
    os_free(he);
    qcom_wps_connect(g_wps.dev, &g_wps.target_ap);
}

static void post_deferred_scan_stop(void)
{
    struct qcom_he_msg *msg = os_zalloc(sizeof(*msg));
    if (!msg) {
        LOG_ERR("post_deferred_scan_stop: out of memory");
        return;
    }
    msg->handle = scan_stop_handle;
    if (qcom_hostap_post(msg) != 0) {
        LOG_ERR("post_deferred_scan_stop: post failed");
        os_free(msg);
    }
}

static void post_deferred_rescan(void)
{
    struct qcom_he_msg *msg = os_zalloc(sizeof(*msg));
    if (!msg) {
        LOG_ERR("post_deferred_rescan: out of memory");
        return;
    }
    msg->handle = rescan_handle;
    if (qcom_hostap_post(msg) != 0) {
        LOG_ERR("post_deferred_rescan: post failed");
        os_free(msg);
    }
}

static void post_deferred_cancel(void)
{
    struct qcom_he_msg *msg = os_zalloc(sizeof(*msg));
    if (!msg) {
        LOG_ERR("post_deferred_cancel: out of memory");
        return;
    }
    msg->handle = cancel_handle;
    if (qcom_hostap_post(msg) != 0) {
        LOG_ERR("post_deferred_cancel: post failed");
        os_free(msg);
    }
}

static void post_deferred_connect(void)
{
    struct qcom_he_msg *msg = os_zalloc(sizeof(*msg));
    if (!msg) {
        LOG_ERR("post_deferred_connect: out of memory");
        return;
    }
    msg->handle = connect_handle;
    if (qcom_hostap_post(msg) != 0) {
        LOG_ERR("post_deferred_connect: post failed");
        os_free(msg);
    }
}

/* ------------------------------------------------------------------ */
/* EAP frame TX                                                         */
/* ------------------------------------------------------------------ */

static int wps_eap_tx(const uint8_t *bssid, uint8_t eap_id,
                      enum wsc_op_code op_code, const struct wpabuf *payload)
{
    /*
     * Build EAP-Response/WSC_MSG (or WSC_Done/WSC_NACK):
     *   EAPOL header (4B) + EAP header (4B) + Expanded type (8B) +
     *   Op-Code (1B) + Flags (1B) + WSC payload
     *
     * For EAP-Response/Identity (no WSC payload):
     *   EAPOL header + EAP header + Identity string
     */
    size_t eap_payload_len = payload ? wpabuf_len(payload) : 0;
    bool is_identity = (op_code == 0xFF); /* special marker for Identity */

    size_t eap_data_len;
    if (is_identity) {
        eap_data_len = 4 + 1 + WSC_ID_ENROLLEE_LEN; /* EAP header + Type + identity */
    } else {
        eap_data_len = 4 + 8 + 2 + eap_payload_len; /* EAP hdr + expanded + op+flags + data */
    }
    size_t total = 4 + eap_data_len; /* EAPOL header + EAP data */

    u8 *buf = os_zalloc(total);
    if (!buf)
        return -ENOMEM;

    /* EAPOL header */
    buf[0] = 0x02;  /* version */
    buf[1] = 0x00;  /* type = EAP */
    WPA_PUT_BE16(buf + 2, (u16)eap_data_len);

    /* EAP header */
    buf[4] = 0x02;  /* Code = Response */
    buf[5] = eap_id;
    WPA_PUT_BE16(buf + 6, (u16)eap_data_len);

    if (is_identity) {
        buf[8] = 0x01; /* Type = Identity */
        os_memcpy(buf + 9, WSC_ID_ENROLLEE, WSC_ID_ENROLLEE_LEN);
    } else {
        /* Expanded type: Vendor-Id=00:37:2a, Vendor-Type=00000001 */
        buf[8]  = 0xfe; /* Type = Expanded */
        buf[9]  = 0x00; buf[10] = 0x37; buf[11] = 0x2a; /* WFA OUI */
        WPA_PUT_BE32(buf + 12, 0x00000001); /* WFA Vendor-Type */
        buf[16] = (u8)op_code;
        buf[17] = 0x00; /* Flags */
        if (payload && eap_payload_len > 0)
            os_memcpy(buf + 18, wpabuf_head(payload), eap_payload_len);
    }

    int ret = -EIO;

    if (g_wps.l2) {
        ret = l2_packet_send(g_wps.l2, bssid, ETH_P_EAPOL, buf, total);
        LOG_DBG("wps_eap_tx: op_code=%u len=%zu ret=%d", op_code, total, ret);
    } else {
        LOG_ERR("wps_eap_tx: l2 handle not initialised");
    }

    os_free(buf);
    return ret;
}

/* ------------------------------------------------------------------ */
/* EAP frame RX hook — called by firmware for every EAP frame          */
/* ------------------------------------------------------------------ */

/*
 * wps_eap_rx_msg — eloop-thread message carrying a copied EAPOL frame.
 * The DPM thread allocates this, copies src_addr + eapol_data, and posts
 * it via qcom_hostap_post().  The eloop thread runs wps_eap_rx_handle()
 * which does the actual crypto work (DH key gen, M1-M8) on its 80 KB stack.
 */
struct wps_eap_rx_msg {
    struct qcom_he_msg  base;           /* MUST be first member */
    uint8_t             src_addr[6];
    uint16_t            eapol_len;
    uint8_t             eapol_data[];   /* flexible array, allocated inline */
};

static int qcom_wps_set_ie(uint8_t device_id, uint8_t frame_type, bool inject);

static void wps_eap_rx_handle(struct qcom_he_msg *base)
{
    struct wps_eap_rx_msg *m = CONTAINER_OF(base, struct wps_eap_rx_msg, base);
    const uint8_t *src_addr   = m->src_addr;
    const uint8_t *eapol_data = m->eapol_data;
    uint16_t       eapol_len  = m->eapol_len;

    LOG_DBG("wps_eap_rx: src=%02x:%02x:%02x len=%u",
            src_addr[0], src_addr[1], src_addr[2], eapol_len);

    if (!g_wps.wps_data) {
        LOG_WRN("wps_eap_rx: wps_data not initialised");
        goto out;
    }

    if (eapol_len < 4) {
        LOG_WRN("wps_eap_rx: frame too short (%u)", eapol_len);
        goto out;
    }

    uint8_t eapol_type = eapol_data[1];
    uint16_t eap_len   = WPA_GET_BE16(eapol_data + 2);

    if (eapol_type != 0x00) {
        LOG_DBG("wps_eap_rx: ignoring EAPOL type=0x%02x", eapol_type);
        goto out;
    }

    if (eapol_len < 4 + eap_len || eap_len < 4) {
        LOG_WRN("wps_eap_rx: invalid EAP length eap_len=%u eapol_len=%u",
                eap_len, eapol_len);
        goto out;
    }

    const uint8_t *eap = eapol_data + 4;
    uint8_t eap_code = eap[0];
    uint8_t eap_id   = eap[1];
    uint8_t eap_type = (eap_len > 4) ? eap[4] : 0;

    LOG_DBG("wps_eap_rx: code=%u id=%u type=0x%02x", eap_code, eap_id, eap_type);

    if (eap_code == 0x01 && eap_type == 0x01) {
        LOG_DBG("wps_eap_rx: EAP-Request/Identity — sending Response/Identity");
        wps_eap_tx(src_addr, eap_id, 0xFF, NULL);
        goto out;
    }

    if (eap_code == 0x01 && eap_type == 0xfe) {
        if (eap_len < 4 + 1 + 3 + 4 + 2) {
            LOG_WRN("wps_eap_rx: EAP-WSC too short");
            goto out;
        }
        uint8_t op_code = eap[4 + 1 + 3 + 4];
        uint8_t flags   = eap[4 + 1 + 3 + 4 + 1];

        size_t hdr_size = 4 + 1 + 3 + 4 + 1 + 1;
        if (flags & 0x01) hdr_size += 2;

        const uint8_t *wsc_data     = eap + hdr_size;
        size_t         wsc_data_len = (eap_len > hdr_size) ? eap_len - hdr_size : 0;

        LOG_DBG("wps_eap_rx: WSC op_code=0x%02x wsc_data_len=%zu",
                op_code, wsc_data_len);

        if (op_code == WSC_Start) {
            LOG_DBG("wps_eap_rx: WSC_Start received — building M1");
            enum wsc_op_code tx_op;
            struct wpabuf *m1 = wps_get_msg(g_wps.wps_data, &tx_op);
            if (m1) {
                LOG_INF("wps_eap_rx: M1 built OK len=%zu — sending", wpabuf_len(m1));
                wps_eap_tx(src_addr, eap_id, tx_op, m1);
                wpabuf_free(m1);
            } else {
                LOG_ERR("wps_eap_rx: wps_get_msg(M1) returned NULL");
                qcom_wps_cancel(g_wps.dev);
            }
            goto out;
        }

        struct wpabuf *msg = wpabuf_alloc_copy(wsc_data, wsc_data_len);
        if (!msg) goto out;

        enum wps_process_res res = wps_process_msg(
                g_wps.wps_data, (enum wsc_op_code)op_code, msg);
        wpabuf_free(msg);

        LOG_DBG("wps_eap_rx: wps_process_msg res=%d state=%d",
                (int)res, g_wps.wps_data->state);

        if (res == WPS_FAILURE) {
            LOG_WRN("wps_eap_rx: WPS_FAILURE");
            qcom_wps_cancel(g_wps.dev);
            goto out;
        }

        if (res == WPS_CONTINUE) {
            enum wsc_op_code tx_op;
            struct wpabuf *resp = wps_get_msg(g_wps.wps_data, &tx_op);
            if (resp) {
                LOG_DBG("wps_eap_rx: sending op_code=0x%02x len=%zu",
                        tx_op, wpabuf_len(resp));
                wps_eap_tx(src_addr, eap_id, tx_op, resp);
                wpabuf_free(resp);

                /* After WSC_Done is sent (state=WPS_FINISHED), trigger
                 * PSK reconnect with the credentials saved in wps_cred_cb. */
                if (g_wps.wps_data &&
                    g_wps.wps_data->state == WPS_FINISHED &&
                    g_wps.cred_valid) {

                    bool            cred_valid = g_wps.cred_valid;
                    struct wps_credential cred = g_wps.cred;
                    uint8_t         device_id  = g_wps.device_id;
                    uint16_t        ap_channel = g_wps.target_ap.channel;

                    qcom_wps_cancel(g_wps.dev);

                    if (cred_valid) {
                        uint16_t wmi_auth;
                        if (cred.auth_type & WPS_AUTH_WPA2PSK)
                            wmi_auth = 0x10;
                        else if (cred.auth_type & WPS_AUTH_WPAPSK)
                            wmi_auth = 0x08;
                        else
                            wmi_auth = 0x01;

                        uint8_t wmi_cipher;
                        if (cred.encr_type & WPS_ENCR_AES)
                            wmi_cipher = 0x08;
                        else if (cred.encr_type & WPS_ENCR_TKIP)
                            wmi_cipher = 0x04;
                        else
                            wmi_cipher = 0x01;

                        LOG_INF("wps_eap_rx: reconnecting SSID=%.*s "
                                "wmi_auth=0x%x cipher=0x%x key_len=%zu",
                                (int)cred.ssid_len, cred.ssid,
                                wmi_auth, wmi_cipher, cred.key_len);

                        qapi_WLAN_Disconnect(device_id);

                        if (wmi_auth != 0x01 && cred.key_len > 0) {
                            const uint8_t *psk     = cred.key;
                            uint8_t        psk_len = (uint8_t)cred.key_len;
                            uint8_t        psk_bin[32];

                            /* WPS Network Key with key_len==64 is a hex-encoded
                             * 32-byte PMK (WPA2 passphrase max is 63 chars).
                             * Convert to binary — firmware expects either an
                             * ASCII passphrase (8-63 bytes) or a 32-byte PMK. */
                            if (cred.key_len == 64) {
                                if (hexstr2bin((const char *)cred.key,
                                               psk_bin, sizeof(psk_bin)) == 0) {
                                    psk     = psk_bin;
                                } else {
                                    LOG_ERR("wps: invalid 64-char PSK, aborting reconnect");
                                    goto out;
                                }
                            }

                            wlan_set_psk_params(device_id,
                                                cred.ssid, cred.ssid_len,
                                                wmi_auth, wmi_cipher,
                                                psk, psk_len);
                        } else {
                            qapi_WLAN_Set_Param(device_id,
                                                __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                                                __QAPI_WLAN_PARAM_GROUP_WIRELESS_SSID,
                                                (void *)cred.ssid, cred.ssid_len, false);
                            wlan_clear_privacy(device_id);
                        }

                        /* Clear WPS IE from Assoc Req before PSK reconnect */
                        qcom_wps_set_ie(device_id, WMI_FRAME_ASSOC_REQ, false);

                        /* Sync cfg_connect so "wifi status" and net_mgmt
                         * events reflect the correct SSID, security and PSK
                         * when station_connect_event fires for the PSK
                         * reconnect. */
                        enum wifi_security_type sec =
                            (cred.auth_type & WPS_AUTH_WPA2PSK) ?
                                WIFI_SECURITY_TYPE_PSK :
                            (cred.auth_type & WPS_AUTH_WPAPSK) ?
                                WIFI_SECURITY_TYPE_WPA_PSK :
                                WIFI_SECURITY_TYPE_NONE;
                        qwifi_wps_sync_connect_params(
                            g_wps.dev,
                            cred.ssid, cred.ssid_len,
                            sec,
                            cred.key_len > 0 ? cred.key : NULL,
                            (uint8_t)cred.key_len);

                        /* Set channel hint from WPS scan result so the PSK
                         * reconnect targets the known AP channel directly.
                         * Use WIRELESS_CHANNEL (pdc channel_hint path) which
                         * supports both 2.4 GHz and 5 GHz via firmware-side
                         * dc_get_chidx_from_freq(). */
                        if (ap_channel > 0) {
                            uint32_t ch_param[2] = { ap_channel, FALSE };
                            qapi_WLAN_Set_Param(device_id,
                                __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                                __QAPI_WLAN_PARAM_GROUP_WIRELESS_CHANNEL,
                                ch_param, sizeof(ch_param), false);
                        }

                        qapi_WLAN_Commit(device_id);
                    }
                    goto out;
                }
            } else {
                LOG_WRN("wps_eap_rx: wps_get_msg returned NULL");
            }
        }
        goto out;
    }

    if (eap_code == 0x04) {
        LOG_DBG("wps_eap_rx: EAP-Failure received (normal WPS completion)");
        goto out;
    }

    LOG_DBG("wps_eap_rx: unhandled EAP code=%u type=0x%02x", eap_code, eap_type);

out:
    os_free(m);
}

/*
 * wps_eap_rx — called directly by the DPM thread (firmware data path).
 * Must NOT do any crypto here (DPM stack is too small for DH key gen).
 * Deep-copy the frame and post to the eloop thread for actual processing.
 */
static void wps_eap_rx(const uint8_t *src_addr,
                        const uint8_t *eapol_data,
                        uint16_t eapol_len)
{
    struct wps_eap_rx_msg *m = os_malloc(sizeof(*m) + eapol_len);
    if (!m) {
        LOG_ERR("wps_eap_rx: out of memory (len=%u)", eapol_len);
        return;
    }
    os_memcpy(m->src_addr, src_addr, 6);
    m->eapol_len = eapol_len;
    os_memcpy(m->eapol_data, eapol_data, eapol_len);
    m->base.handle = wps_eap_rx_handle;

    if (qcom_hostap_post(&m->base) != 0) {
        LOG_ERR("wps_eap_rx: qcom_hostap_post failed");
        os_free(m);
    }
}

/* ------------------------------------------------------------------ */
/* Credential callback                                                  */
/* ------------------------------------------------------------------ */

static int wps_cred_cb(void *ctx, const struct wps_credential *cred)
{
    ARG_UNUSED(ctx);

    LOG_INF("wps_cred_cb: SSID=%.*s auth=0x%04x encr=0x%04x key_len=%zu",
            (int)cred->ssid_len, cred->ssid,
            cred->auth_type, cred->encr_type, cred->key_len);

    g_wps.wps_success = true;

    /* Save credentials — reconnect happens after WPS session is fully
     * cancelled (in WPS_DONE path) to avoid conflicting with the ongoing
     * EAP-WSC session and WPS IE still injected in Assoc Req. */
    g_wps.cred       = *cred;
    g_wps.cred_valid = true;

    return 0;
}

/* ------------------------------------------------------------------ */
/* qwifi_wps_scan_event — overrides __weak stub in qcom_wifi_drv.c    */
/* ------------------------------------------------------------------ */

void qwifi_wps_scan_event(uint32_t event_id, void *payload, uint32_t payload_len)
{
    switch (event_id) {

    case QAPI_WLAN_WPS_SCAN_AP_CB_E: {
        if (!payload || payload_len < sizeof(qapi_WLAN_WPS_Scan_AP_Result_t))
            break;
        if (!g_wps.scanning)
            break;

        const qapi_WLAN_WPS_Scan_AP_Result_t *ap = payload;

        /* Already found overlap this round — ignore further AP events */
        if (g_wps.wps_overlap)
            break;

        /* Parse WSC IE */
        wsc_ie_info_t info = {0};
        if (ap->wsc_ie_len > 0)
            info = parse_wsc_ie(ap->wsc_ie, ap->wsc_ie_len);

        LOG_DBG("WPS AP: %02x:%02x:%02x:%02x:%02x:%02x ch=%u RSSI=%d "
                "SSID=%.*s pbc_active=%d",
                ap->bssid[0], ap->bssid[1], ap->bssid[2],
                ap->bssid[3], ap->bssid[4], ap->bssid[5],
                ap->channel, ap->rssi,
                ap->ssid_len, ap->ssid,
                info.pbc_active);

        if (!info.pbc_active)
            break;

        /* BSSID filter: when a target BSSID is specified, ignore all other
         * APs for both overlap counting and connect target selection. */
        if (!is_zero_ether_addr(g_wps.scan_params.bssid) &&
            memcmp(ap->bssid, g_wps.scan_params.bssid, __QAPI_WLAN_MAC_LEN) != 0)
            break;

        /* Channel filter: when a channel list is specified, ignore APs on
         * other channels.  This is a host-side safety check that mirrors
         * the firmware-side dc_clear_scan_list/dc_update_scan_list filter
         * applied in wmi_start_wps_scan_cmd — both must agree on which
         * channels are valid to avoid inconsistent overlap detection. */
        if (g_wps.scan_params.channel_count > 0) {
            bool ch_match = false;
            for (uint8_t i = 0; i < g_wps.scan_params.channel_count; i++) {
                if (ap->channel == g_wps.scan_params.channels[i]) {
                    ch_match = true;
                    break;
                }
            }
            if (!ch_match) {
                LOG_DBG("WPS AP ch=%u not in channel list, skipping", ap->channel);
                break;
            }
        }

        /* UUID dedup */
        if (!info.has_uuid) {
            /* No UUID — treat as distinct registrar (conservative) */
            g_wps.pbc_no_uuid_count++;
        } else {
            bool dup = false;
            for (uint8_t i = 0; i < g_wps.pbc_uuid_count; i++) {
                if (memcmp(g_wps.pbc_uuid[i], info.uuid, WPS_UUID_LEN) == 0) {
                    dup = true;
                    break;
                }
            }
            if (!dup && g_wps.pbc_uuid_count < WPS_MAX_PBC_REGISTRAR) {
                memcpy(g_wps.pbc_uuid[g_wps.pbc_uuid_count],
                       info.uuid, WPS_UUID_LEN);
                g_wps.pbc_uuid_count++;
            }
        }

        /* Store first PBC-active AP as connect target */
        if (!g_wps.target_found) {
            memcpy(g_wps.target_ap.bssid, ap->bssid, 6);
            g_wps.target_ap.ssid_len = ap->ssid_len;
            memcpy(g_wps.target_ap.ssid, ap->ssid, ap->ssid_len);
            g_wps.target_ap.channel  = ap->channel;
            g_wps.target_ap.rssi     = ap->rssi;
            g_wps.target_found = true;
        }

        /* Check overlap immediately after each new PBC AP */
        if (distinct_pbc_count() >= 2) {
            LOG_WRN("WPS PBC overlap detected (%u registrars), stopping scan",
                    distinct_pbc_count());
            g_wps.wps_overlap = true;
            /*
             * Cannot call qapi_WLAN_WPS_Scan(STOP) here — running under
             * wlan_qapi_cxt_mutex which wmi_wps_scan() also acquires.
             * Post to eloop thread via qcom_hostap_post() so the call
             * happens after the mutex is released.
             */
            post_deferred_scan_stop();
        }
        break;
    }

    case QAPI_WLAN_WPS_SCAN_COMP_CB_E: {
        if (!g_wps.scanning)
            break;

        g_wps.scanning = false;

        if (g_wps.wps_overlap) {
            LOG_WRN("WPS scan complete: OVERLAP — %u distinct PBC registrars, "
                    "aborting per WSC spec", distinct_pbc_count());
            post_deferred_cancel();
            break;
        }

        if (!g_wps.target_found) {
            /* NOT_FOUND — post rescan to eloop thread (mutex constraint) */
            LOG_DBG("WPS scan complete: NOT_FOUND, retrying scan");
            post_deferred_rescan();
            break;
        }

        /* FOUND — post connect to eloop thread (mutex constraint) */
        LOG_INF("WPS scan complete: FOUND AP %02x:%02x:%02x:%02x:%02x:%02x "
                "SSID=%.*s ch=%u",
                g_wps.target_ap.bssid[0], g_wps.target_ap.bssid[1],
                g_wps.target_ap.bssid[2], g_wps.target_ap.bssid[3],
                g_wps.target_ap.bssid[4], g_wps.target_ap.bssid[5],
                g_wps.target_ap.ssid_len,  g_wps.target_ap.ssid,
                g_wps.target_ap.channel);
        post_deferred_connect();
        break;
    }

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int qcom_wps_start_pbc(const struct device *dev,
                        const uint8_t  *bssid,
                        const uint16_t *channels,
                        uint8_t         channel_count)
{
    if (!dev)
        return -EINVAL;

    if (g_wps.supp_pbc_active) {
        LOG_WRN("qcom_wps_start_pbc: WPS already in progress");
        return -EBUSY;
    }

    g_wps.dev             = dev;
    g_wps.supp_pbc_active = true;
    g_wps.wps_success     = false;

    /* Build scan_params — persists for the full PBC session (all rescan rounds) */
    memset(&g_wps.scan_params, 0, sizeof(g_wps.scan_params));
    g_wps.scan_params.op = QAPI_WLAN_WPS_SCAN_START_PBC_E;
    if (bssid)
        memcpy(g_wps.scan_params.bssid, bssid, __QAPI_WLAN_MAC_LEN);
    if (channels && channel_count > 0) {
        uint8_t n = (channel_count > QAPI_WLAN_WPS_SCAN_MAX_CHANNELS)
                    ? QAPI_WLAN_WPS_SCAN_MAX_CHANNELS : channel_count;
        g_wps.scan_params.channel_count = n;
        memcpy(g_wps.scan_params.channels, channels, n * sizeof(uint16_t));
    }

    /* Start PBC walk timer — covers entire scan + connect + M1-M8 window.
     * eloop_register_timeout modifies eloop internals without locking,
     * so acquire the hostap glue lock when calling from non-eloop thread.
     */
    qcom_hostap_lock();
    eloop_register_timeout(WPS_PBC_WALK_TIME, 0, pbc_walk_timer_fn, NULL, NULL);
    qcom_hostap_unlock();

    return qcom_wps_scan(dev);
}

/* ------------------------------------------------------------------ */
/* WPS Probe Request IE inject / remove                                 */
/* ------------------------------------------------------------------ */

/*
 * qcom_wps_set_ie() — inject or remove a WPS IE for a given management frame.
 *
 * inject=true : build the WSC IE via hostap and send it to firmware.
 *               PROBE_REQ uses wps_build_probe_req_ie (full device info).
 *               ASSOC_REQ uses wps_build_assoc_req_ie (RequestType only).
 * inject=false: send a single 0xdd byte to remove the IE from firmware.
 */
static int qcom_wps_set_ie(uint8_t device_id, uint8_t frame_type, bool inject)
{
    if (!g_wps.wps_ctx) {
        LOG_ERR("qcom_wps_set_ie: WPS not initialised");
        return -EINVAL;
    }

    qapi_WLAN_App_Ie_Params_t ie = {0};
    ie.mgmt_Frame_Type = frame_type;

    struct wpabuf *ie_buf = NULL;

    if (inject) {
        if (frame_type == WMI_FRAME_PROBE_REQ) {
            ie_buf = wps_build_probe_req_ie(DEV_PW_PUSHBUTTON,
                                            &g_wps.wps_ctx->dev,
                                            g_wps.wps_ctx->uuid,
                                            WPS_REQ_ENROLLEE, 0, NULL);
        } else {
            ie_buf = wps_build_assoc_req_ie(WPS_REQ_ENROLLEE);
        }
        if (!ie_buf) {
            LOG_ERR("qcom_wps_set_ie: build IE failed (frame_type=%u)", frame_type);
            return -ENOMEM;
        }
        ie.ie_Info = (uint8_t *)wpabuf_head(ie_buf);
        ie.ie_Len  = (uint8_t)wpabuf_len(ie_buf);
    } else {
        static const uint8_t clear_ie[] = { 0xdd };
        ie.ie_Info = (uint8_t *)clear_ie;
        ie.ie_Len  = 1;
    }

    qapi_Status_t ret = qapi_WLAN_Set_Param(device_id,
                                             __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                                             __QAPI_WLAN_PARAM_GROUP_WIRELESS_APP_IE,
                                             &ie, sizeof(ie), false);
    wpabuf_free(ie_buf);

    if (ret != QAPI_OK) {
        LOG_ERR("qcom_wps_set_ie: failed (frame_type=%u inject=%d ret=%d)",
                frame_type, (int)inject, ret);
        return -EIO;
    }
    LOG_DBG("WPS IE %s (frame_type=%u bytes=%u)",
            inject ? "injected" : "removed", frame_type, ie.ie_Len);
    return 0;
}

static int qcom_wps_scan(const struct device *dev)
{
    if (!dev)
        return -EINVAL;

    if (g_wps.scanning) {
        LOG_WRN("qcom_wps_scan: scan already in progress");
        return -EBUSY;
    }

    g_wps.dev      = dev;
    g_wps.scanning = true;

    /* Inject WPS PBC Probe Request IE — must be set before starting scan */
    if (qcom_wps_set_ie(QCOM_DEV_STA_ID, WMI_FRAME_PROBE_REQ, true) < 0) {
        g_wps.scanning = false;
        return -EIO;
    }

    qapi_Status_t ret = qapi_WLAN_WPS_Scan(0, &g_wps.scan_params);
    if (ret != QAPI_OK) {
        g_wps.scanning = false;
        qcom_wps_set_ie(QCOM_DEV_STA_ID, WMI_FRAME_PROBE_REQ, false);
        LOG_ERR("qcom_wps_scan: qapi_WLAN_WPS_Scan failed (%d)", ret);
        return -EIO;
    }

    return 0;
}

int qcom_wps_connect(const struct device *dev,
                     const struct qcom_wps_scan_result *result)
{
    if (!dev || !result)
        return -EINVAL;

    /* WPS enrollee always uses the STA device */
    const uint8_t device_id = QCOM_DEV_STA_ID;

    memcpy(g_wps.ap_bssid, result->bssid, 6);
    g_wps.device_id = device_id;

    LOG_INF("qcom_wps_connect: BSSID=%02x:%02x:%02x:%02x:%02x:%02x "
            "SSID=%.*s ch=%u",
            result->bssid[0], result->bssid[1], result->bssid[2],
            result->bssid[3], result->bssid[4], result->bssid[5],
            result->ssid_len, result->ssid, result->channel);

    /* Set SSID */
    qapi_WLAN_Set_Param(device_id,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_SSID,
                        (void *)result->ssid, result->ssid_len, false);

    /* Lock to target BSSID — prevents associating to a wrong AP */
    qapi_WLAN_Set_Param(device_id,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_BSSID,
                        (void *)result->bssid, __QAPI_WLAN_MAC_LEN, false);

    /* Set channel to speed up association */
    if (result->channel > 0) {
        uint32_t channel[2] = { result->channel, 0 };
        qapi_WLAN_Set_Param(device_id,
                            __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                            __QAPI_WLAN_PARAM_GROUP_WIRELESS_CHANNEL,
                            (void *)&channel, sizeof(channel), false);
    }

    wlan_clear_privacy(device_id);
    /* Set connect_pending flag in firmware so discovery.c bypasses strict
     * profile matching during WPS open association. */
    wlan_set_wps_open_connect(device_id, result->channel);

    /*
     * Inject WSC IE into Association Request — required by WSC 2.0 spec
     * section 7.2.  Must be set BEFORE qapi_WLAN_Commit() so firmware
     * includes it in the Assoc Request frame.
     */
    if (qcom_wps_set_ie(device_id, WMI_FRAME_ASSOC_REQ, true) < 0) {
        LOG_WRN("qcom_wps_connect: assoc req IE inject failed");
    }

    /* Commit — triggers 802.11 authentication + association */
    qapi_WLAN_Commit(device_id);

    return 0;
}

bool qcom_wps_connect_in_progress(void)
{
    return g_wps.supp_pbc_active && !g_wps.scanning;
}

int qcom_wps_init(const struct device *dev)
{
    if (g_wps.wps_ctx) {
        LOG_WRN("qcom_wps_init: already initialised");
        return 0;
    }

    struct wps_context *wps = os_zalloc(sizeof(*wps));
    if (!wps) {
        LOG_ERR("qcom_wps_init: out of memory");
        return -ENOMEM;
    }

    /* Enrollee-only — AP and registrar roles not used */
    wps->ap        = 0;
    wps->registrar = NULL;

    /* WPS state: CONFIGURED means the device already has network credentials */
    wps->wps_state = WPS_STATE_CONFIGURED;

    /* config_methods: include all PBC variants so modern APs accept M1.
     * WSC 2.0 APs expect Virtual or Physical PushButton (0x0280 or 0x0480).
     * Legacy PBC (0x0080) alone causes some APs to respond with M2D.
     */
    wps->config_methods = WPS_CONFIG_PUSHBUTTON |
                          WPS_CONFIG_VIRT_PUSHBUTTON |
                          WPS_CONFIG_PHY_PUSHBUTTON;

    /* Security: WPA2-PSK / AES */
    wps->auth_types = WPS_AUTH_WPA2PSK;
    wps->encr_types = WPS_ENCR_AES;

    /*
     * Device info — configurable via Kconfig (WIFI_QCOM_WPS sub-options).
     * These strings appear in the WPS IE and M1 message sent to the AP.
     * os_strdup() is used because wps_device_data fields are char* (not
     * const char*), and Kconfig strings are string literals — consistent
     * with how wpas_wps_init() sets these fields in wpa_supplicant.
     */
    wps->dev.device_name   = os_strdup(CONFIG_WPS_DEVICE_NAME);
    wps->dev.manufacturer  = os_strdup(CONFIG_WPS_MANUFACTURER);
    wps->dev.model_name    = os_strdup(CONFIG_WPS_MODEL_NAME);
    wps->dev.model_number  = os_strdup(CONFIG_WPS_MODEL_NUMBER);
    wps->dev.serial_number = os_strdup(CONFIG_WPS_SERIAL_NUMBER);

    if (!wps->dev.device_name || !wps->dev.manufacturer ||
        !wps->dev.model_name  || !wps->dev.model_number ||
        !wps->dev.serial_number) {
        LOG_ERR("qcom_wps_init: out of memory for device info");
        os_free(wps->dev.device_name);
        os_free(wps->dev.manufacturer);
        os_free(wps->dev.model_name);
        os_free(wps->dev.model_number);
        os_free(wps->dev.serial_number);
        os_free(wps);
        return -ENOMEM;
    }

    /*
     * UUID-E: RFC 4122 version 5 (name-based SHA-1), derived from the
     * station MAC address via uuid_gen_mac_addr().  This is the same
     * method used by wpa_supplicant (wpas_wps_init).
     * Also copy MAC into dev.mac_addr so M1 carries the correct address.
     */
    {
        struct net_if *iface = net_if_get_first_wifi();
        if (iface) {
            const struct net_linkaddr *la = net_if_get_link_addr(iface);
            if (la && la->len == ETH_ALEN) {
                uuid_gen_mac_addr(la->addr, wps->uuid);
                os_memcpy(wps->dev.mac_addr, la->addr, ETH_ALEN);
            }
        }
    }

    /* Callbacks — wps_cred_cb, wps_event_cb, rf_band_cb are static in this file */
    wps->cred_cb     = wps_cred_cb;
    wps->event_cb    = wps_event_cb;
    wps->rf_band_cb  = wps_rf_band_cb;
    wps->cb_ctx      = (void *)dev;

    /*
     * RF bands supported by QCC730: 2.4 GHz + 5 GHz.
     * WPS_RF_24GHZ = 0x01, WPS_RF_50GHZ = 0x02 (wps_defs.h)
     * This is the default advertised in M1; the actual band used
     * for the current connection is returned by rf_band_cb at build time.
     */
    wps->dev.rf_bands = WPS_RF_24GHZ | WPS_RF_50GHZ;

    /* Sync config_methods to wps_device_data so wps_build_probe_req_ie
     * and wps_build_assoc_req_ie pick them up correctly.               */
    wps->dev.config_methods = wps->config_methods;

    g_wps.wps_ctx = wps;
    g_wps.dev     = dev;

    LOG_DBG("qcom_wps_init: WPS context initialised");
    return 0;
}

void qcom_wps_deinit(const struct device *dev)
{
    ARG_UNUSED(dev);

    /* Cancel any in-progress session first.
     * Acquire hostap lock because qcom_wps_cancel calls eloop_cancel_timeout
     * which is not thread-safe — must run under g_he_lock from non-eloop thread.
     */
    if (g_wps.supp_pbc_active) {
        qcom_hostap_lock();
        qcom_wps_cancel(dev);
        qcom_hostap_unlock();
    }

    if (g_wps.wps_ctx) {
        /* Free os_strdup'd device info strings */
        os_free(g_wps.wps_ctx->dev.device_name);
        os_free(g_wps.wps_ctx->dev.manufacturer);
        os_free(g_wps.wps_ctx->dev.model_name);
        os_free(g_wps.wps_ctx->dev.model_number);
        os_free(g_wps.wps_ctx->dev.serial_number);
        os_free(g_wps.wps_ctx);
        g_wps.wps_ctx = NULL;
    }

    LOG_DBG("qcom_wps_deinit: WPS context freed");
}

void qcom_wps_cancel(const struct device *dev)
{
    ARG_UNUSED(dev);

    if (!g_wps.supp_pbc_active) {
        return;
    }

    /* Stop firmware WPS scan if one is in progress */
    if (g_wps.scanning) {
        g_wps.scanning = false;
        qapi_WLAN_WPS_Scan_Params_t stop = { .op = QAPI_WLAN_WPS_SCAN_STOP_E };
        qapi_WLAN_WPS_Scan(0, &stop);
    }

    /* Remove WPS IEs from both Probe Request and Association Request */
    qcom_wps_set_ie(g_wps.device_id, WMI_FRAME_PROBE_REQ, false);
    qcom_wps_set_ie(g_wps.device_id, WMI_FRAME_ASSOC_REQ, false);

    /*
     * eloop_cancel_timeout modifies eloop internals without locking.
     * qcom_wps_cancel is called from two contexts:
     *   a) eloop thread (cancel_handle): g_he_lock already held — do NOT
     *      call qcom_hostap_lock() again (non-recursive mutex, would deadlock)
     *   b) non-eloop thread (qwifi_drv_wps_config, qcom_wps_deinit): must
     *      acquire lock first. Callers are responsible for this.
     */
    eloop_cancel_timeout(pbc_walk_timer_fn, ELOOP_ALL_CTX, ELOOP_ALL_CTX);
    eloop_cancel_timeout(session_timer_fn,  ELOOP_ALL_CTX, ELOOP_ALL_CTX);

    nt_dpm_set_eap_enterprise_hook(NULL);

    /* Remove ENC_NONE DPM STA entry if it was added */
    if (g_wps.eap_sta_added) {
        nt_dpm_delete_sta(g_wps.eap_staid);
        g_wps.eap_sta_added = false;
        g_wps.eap_staid = 0;
        LOG_DBG("qcom_wps_cancel: ENC_NONE DPM STA removed");
    }

    if (g_wps.wps_data) {
        wps_deinit(g_wps.wps_data);
        g_wps.wps_data = NULL;
    }

    if (g_wps.l2) {
        l2_packet_deinit(g_wps.l2);
        g_wps.l2 = NULL;
    }

    /* Disconnect open WPS association if WPS did not succeed.
     * Capture wps_success before memset clears g_wps below.
     * On success the PSK reconnect path has already triggered via
     * WPS_CONTINUE + state==WPS_FINISHED. */
    bool succeeded = g_wps.wps_success;

    /* Clear connect_pending flag in firmware */
    wlan_clear_wps_open_connect();

    /* Preserve wps_ctx — it lives for the interface lifetime (qcom_wps_deinit) */
    struct wps_context *saved_ctx = g_wps.wps_ctx;
    const struct device *saved_dev = g_wps.dev;
    uint8_t saved_device_id = g_wps.device_id;
    memset(&g_wps, 0, sizeof(g_wps));
    g_wps.wps_ctx = saved_ctx;
    g_wps.dev     = saved_dev;

    if (!succeeded) {
        qapi_WLAN_Disconnect(saved_device_id);
    }

    LOG_DBG("qcom_wps_cancel: WPS cancelled");
}

void qcom_wps_assoc_event(const struct device *dev, const uint8_t *bssid,
                           bool success, uint8_t device_id)
{
    if (!success) {
        LOG_WRN("qcom_wps_assoc_event: assoc failed");
        qcom_wps_cancel(dev);
        return;
    }

    g_wps.dev       = dev;
    g_wps.device_id = device_id;
    memcpy(g_wps.ap_bssid, bssid, 6);

    LOG_INF("qcom_wps_assoc_event: associated to %02x:%02x:%02x:%02x:%02x:%02x",
            bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);

    /*
     * Initialise per-session wps_data via hostap wps_init().
     * wps_ctx was created in qcom_wps_init() and holds device info + callbacks.
     * wps_data must be non-NULL before the EAP hook processes incoming frames.
     *
     * UUID-E: derive from interface MAC address (RFC 4122 name-based variant).
     * For now use the MAC address bytes directly padded to 16 bytes — a proper
     * uuid_gen_mac_addr() derivation can replace this later.
     */
    if (!g_wps.wps_ctx) {
        LOG_ERR("qcom_wps_assoc_event: wps_ctx not initialised");
        qcom_wps_cancel(dev);
        return;
    }

    {
        struct wps_config cfg = {0};
        cfg.wps  = g_wps.wps_ctx;
        cfg.pbc  = 1;
        cfg.registrar = 0;
        cfg.peer_addr = bssid;

        g_wps.wps_data = wps_init(&cfg);
        if (!g_wps.wps_data) {
            LOG_ERR("qcom_wps_assoc_event: wps_init failed");
            qcom_wps_cancel(dev);
            return;
        }
        LOG_DBG("qcom_wps_assoc_event: wps_data initialised");
    }

    /* Register EAP hook — from this point firmware delivers EAP-WSC frames */
    nt_dpm_set_eap_enterprise_hook(wps_eap_rx);

    /*
     * Add temporary ENC_NONE DPM STA entry so outbound EAPOL frames
     * (EAPOL-Start, EAP-Response) can be routed over-the-air.
     * Without this entry, nt_dpm_process_eth_packet_from_stack() cannot
     * find a matching STA and drops all EAPOL TX frames silently.
     * Mirrors qcom_ent_open_eap_tx() in qcom_wifi_enterprise_glue.c.
     */
    {
        wps_sta_cfg_t cfg = {0};
        memcpy(cfg.bssid,           bssid, sizeof(cfg.bssid));
        cfg.IsAP = 1;
        memcpy(cfg.sta_mac_address, bssid, sizeof(cfg.sta_mac_address));
        cfg.qos_sta = 1;
        cfg.sec_mode = 0;   /* ENC_NONE */
        cfg.ht = 1;
        int err = nt_dpm_add_sta(&cfg, &g_wps.eap_staid, 0);
        if (err == 0) {
            g_wps.eap_sta_added = true;
            LOG_DBG("qcom_wps_assoc_event: ENC_NONE DPM STA added staid=%u",
                    g_wps.eap_staid);
        } else {
            LOG_ERR("qcom_wps_assoc_event: nt_dpm_add_sta failed err=%d", err);
        }
    }

    /*
     * Send EAPOL-Start to trigger AP to initiate EAP exchange.
     * WSC 2.0 spec section 7.4.1: after successful assoc, enrollee SHALL
     * send EAPOL-Start.  AP responds with EAP-Request/Identity, then
     * EAP-Request/WSC_Start.
     *
     * Use l2_packet (AF_PACKET raw socket) — the same mechanism wpa_supplicant
     * uses for EAPOL TX.  net_if_send_data() walks the IP stack and cannot
     * carry Ethertype 0x888E frames directly.
     *
     * EAPOL-Start frame payload (after Ethernet header):
     *   Version=0x02, Type=0x01(Start), Length=0x0000
     */
    {
        static const u8 eapol_start[] = { 0x02, 0x01, 0x00, 0x00 };
        char ifname[16] = {0};
        struct net_if *iface = net_if_get_first_wifi();

        if (iface && net_if_get_name(iface, ifname, sizeof(ifname)) > 0) {
            /* Initialise persistent l2 handle for the EAP-WSC session.
             * Reused by wps_eap_tx() for all M1-M8 responses; freed in
             * qcom_wps_cancel() when the session ends. */
            g_wps.l2 = l2_packet_init(ifname, NULL, ETH_P_EAPOL,
                                       NULL, NULL, 0);
            if (g_wps.l2) {
                int ret = l2_packet_send(g_wps.l2, bssid, ETH_P_EAPOL,
                                         eapol_start, sizeof(eapol_start));
                if (ret < 0)
                    LOG_ERR("qcom_wps_assoc_event: EAPOL-Start send failed ret=%d", ret);
                else
                    LOG_DBG("qcom_wps_assoc_event: EAPOL-Start sent via l2_packet (%s)", ifname);
            } else {
                LOG_ERR("qcom_wps_assoc_event: l2_packet_init failed for iface=%s", ifname);
            }
        } else {
            LOG_ERR("qcom_wps_assoc_event: cannot get wifi iface name");
        }
    }

    /* Start session timer — acquire hostap lock (non-eloop thread) */
    qcom_hostap_lock();
    eloop_register_timeout(WPS_PBC_WALK_TIME, 0, session_timer_fn, NULL, NULL);
    qcom_hostap_unlock();

    LOG_DBG("qcom_wps_assoc_event: EAP hook registered, waiting for WSC_Start");
}
