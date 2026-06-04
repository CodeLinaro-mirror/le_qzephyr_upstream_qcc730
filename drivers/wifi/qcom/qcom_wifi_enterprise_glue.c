/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/logging/log.h>

#include "driver_zephyr.h"
#include "supp_api.h"

#include <qwifi_api.h>

/*
 * wpa_supplicant_event_wrapper — async event dispatch to wpa_supplicant thread.
 * Defined in modules/lib/hostap/src/drivers/driver_zephyr.c.
 * Declared here to avoid pulling in the full upstream header tree.
 */
extern void wpa_supplicant_event_wrapper(void *ctx, enum wpa_event_type event,
					 union wpa_event_data *data);

/* Forward declarations — defined in prop/libwifiqcc730/dpm/src/mlme_al.c.
 * Declared here to avoid pulling in the full proprietary header tree.
 */
extern void nt_dpm_set_eap_enterprise_hook(void (*fn)(const uint8_t *src_addr,
						       const uint8_t *eapol_data,
						       uint16_t eapol_len));

/*
 * Mirror of sta_config_t from prop/libwifiqcc730/dpm/inc/mlme_al.h.
 * Layout must stay in sync with the original (all uint8_t — no padding).
 * sec_mode values: NONE=0, WEP40=1, WEP104=2, AES=3, TKIP=4
 */
typedef struct {
    uint8_t bssid[6];
    uint8_t IsAP;            /* 1 = peer is an AP (we are STA) */
    uint8_t sta_mac_address[6];
    uint8_t sta_sig;
    uint8_t dpu_sig;
    uint8_t qos_sta;         /* 1 = QoS STA */
    uint8_t sec_mode;        /* 0=NONE 1=WEP40 2=WEP104 3=AES 4=TKIP */
    uint8_t rmf;             /* 0=no PMF */
    uint8_t ht;              /* 1=HT capable */
} qcom_ent_sta_cfg_t;

#define QCOM_ENT_ENC_NONE 0U
#define QCOM_ENT_ENC_AES  3U  /* sec_mode=WLAN_AES_CRYPT (mlme_al.h) */

#define HS_KEY_USAGE_PTK  0   /* PAIRWISE_USAGE — PTK phase */
#define HS_KEY_USAGE_GTK  1   /* GROUP_USAGE   — GTK phase */

/* nt_dpm_add_sta / nt_dpm_delete_sta — DPM STA table management.
 * Defined in prop/libwifiqcc730/dpm/src/mlme_al.c.
 * Return 0 (ERR_NONE) on success, non-zero on error.
 */
extern int nt_dpm_add_sta(qcom_ent_sta_cfg_t *cfg, uint8_t *staid, uint8_t hal_sta_idx);
extern int nt_dpm_delete_sta(uint8_t staid);

#if !defined(CONFIG_WIFI_QCOM_ENTERPRISE) || !defined(CONFIG_WIFI_NM_WPA_SUPPLICANT)
#error "qcom_wifi_enterprise_glue.c requires CONFIG_WIFI_QCOM_ENTERPRISE && CONFIG_WIFI_NM_WPA_SUPPLICANT"
#endif

LOG_MODULE_REGISTER(qwifi_ent_glue, CONFIG_WIFI_LOG_LEVEL);

struct qcom_ent_ctx {
	void *supp_drv_if_ctx;     /* zep_drv_if_ctx * from driver_zephyr.c */
	struct zep_wpa_supp_dev_callbk_fns cb;
	char ifname[16];
	const struct device *dev;  /* Zephyr device pointer, stored on first assoc */
	uint8_t device_id;         /* QAPI device ID (dev_data->active_device) */
	uint8_t bssid[6];          /* BSSID of current association */
	uint8_t ssid[SSID_MAX_LEN];/* SSID saved at setup time for get_ssid() */
	uint8_t ssid_len;
	struct net_if *iface;      /* net_if for the station — stored at assoc time */
	/* EAP TX path: temporary ENC_NONE DPM STA entry active during EAP auth */
	uint8_t early_staid;
	bool    early_sta_added;
	/* Data path: AES DPM STA entry added after 4-way HS for encrypted data TX */
	uint8_t data_staid;
	bool    data_sta_added;
	/*
	 * hs_compl_ptk_ran: set true by __wrap_hs_compl_evt when the PTK phase
	 * (key_usage=PAIRWISE_USAGE) is intercepted.  Indicates that firmware's
	 * hs_compl_evt() has already called nt_dpm_add_sta() with the correct
	 * bss_sta_idx from halBssInfo.  When this flag is set, pmk_4way_timer_fn
	 * skips qcom_ent_open_data_tx() to avoid adding a duplicate DPM entry
	 * with hal_sta_idx=0 (wrong hardware key slot).
	 *
	 * If __wrap_hs_compl_evt is never called (e.g. intra-archive --wrap miss),
	 * this stays false and the timer falls back to adding the entry with
	 * hal_sta_idx=0 as before.
	 */
	bool    hs_compl_ptk_ran;
	/*
	 * Fallback timer: 600ms after PMK delivery, if FOURWAY_HANDSHAKE_SUCCESS
	 * has not yet arrived (data_sta_added still false), raise the connect event
	 * and start DHCP directly.
	 *
	 * Root cause (Build 13): firmware hs_compl_evt() completes M1→M4 and sends
	 * FOURWAY_HANDSHAKE_SUCCESS via wlan_wmi_cnx_event() → nt_osal_queue_send().
	 * The WMI event queue was full (flooded by premature connect event from
	 * wpa_supplicant.c:WPA_COMPLETED) → event silently dropped (timeout=0).
	 *
	 * Fix (Build 14): premature connect event is suppressed for
	 * WPA_DRIVER_FLAGS_4WAY_HANDSHAKE_8021X drivers.  FOURWAY_HANDSHAKE_SUCCESS
	 * now arrives normally.  This timer remains as a safety net only.
	 *
	 * EAP-TLS note (Build 42): RSA 3072-bit operations during TLS states 0–15
	 * generate elevated WMI event traffic.  The FOURWAY_HANDSHAKE_SUCCESS WMI
	 * event (nt_osal_queue_send timeout=0) can be dropped under this load.
	 * The timer fires and uses hs_compl_ptk_ran to avoid a duplicate DPM entry.
	 */
	struct k_work_delayable pmk_4way_timer;
	/*
	 * connect_raised: set true the first time wifi_mgmt_raise_connect_result_event()
	 * is called for this association — either by pmk_4way_timer_fn (fallback path)
	 * or by qcom_ent_4way_hs_done (normal FOURWAY_HANDSHAKE_SUCCESS path).
	 *
	 * Guards against a duplicate Connected event in the race where the fallback
	 * timer fires before M1 arrives AND FOURWAY_HANDSHAKE_SUCCESS still arrives
	 * later (both paths fire in the same association).  Without this flag,
	 * qcom_ent_4way_hs_done() raises Connected and restarts DHCP unconditionally,
	 * producing two concurrent DHCP sessions that drive a BA del/add storm and
	 * eventually hang the system.
	 *
	 * Reset to false in qcom_ent_assoc_event() on each new association.
	 */
	bool connect_raised;
	/*
	 * timer_added_fallback_staid: set true when pmk_4way_timer_fn calls
	 * qcom_ent_open_data_tx() with hal_sta_idx=0 (hs_compl_ptk_ran was false
	 * at fire time, so the real firmware PTK had not yet run).
	 *
	 * When qcom_ent_4way_hs_done() later detects both this flag AND
	 * hs_compl_ptk_ran=true (firmware PTK did run after the timer), the DPM
	 * table has two AES entries: staid[data_staid] from the timer fallback and
	 * staid[N] from __real_hs_compl_evt.  nt_dpm_find_sta_entry_for_eth_pkt()
	 * returns the lower staid for TX while BA sessions live on the higher staid,
	 * causing nt_dpm_reorder_queue_flush() to deadlock on the reorder mutex →
	 * permanent BA del/add loop → system hang.
	 *
	 * Fix: qcom_ent_4way_hs_done() removes the stale timer entry via
	 * qcom_ent_close_data_tx() when this condition is detected.
	 */
	bool timer_added_fallback_staid;
};

static struct qcom_ent_ctx g_ent_ctx;

/* ---------- hs_compl_evt wrapper (--wrap=hs_compl_evt) ----------
 *
 * The linker --wrap=hs_compl_evt option causes all calls to hs_compl_evt()
 * to route through __wrap_hs_compl_evt instead.  __real_hs_compl_evt
 * resolves to the original cnxmgmt.c implementation which:
 *   - PTK phase (key_usage=0): calls nt_dpm_add_sta() with the correct
 *     bss_sta_idx from dev->halBssInfo to add the AES DPM entry for data TX.
 *   - GTK phase (key_usage=1): sends FOURWAY_HANDSHAKE_SUCCESS via WMI event
 *     → station_connect_event() → qcom_ent_4way_hs_done().
 *
 * This wrapper is DIAGNOSTIC ONLY: log key type and status to confirm
 * hs_compl_evt is called and which phase is executing.
 *
 * DO NOT call qcom_ent_open_data_tx() here.  The firmware PTK phase in
 * __real_hs_compl_evt already adds the AES DPM entry with the correct
 * bss_sta_idx (hardware key slot).  Adding a second entry with hal_sta_idx=0
 * creates a duplicate staid pointing at the wrong key slot → DXE H2H wait
 * timeout → DHCP TX fails.  (Root cause confirmed in Build 18 log:
 * staid[0] from firmware PTK + staid[1] hal_sta_idx=0 from this call.)
 *
 * On GTK phase completion, cancel the PMK fallback timer so it does not fire
 * 600ms later and attempt to add another spurious duplicate DPM entry.
 *
 * WMI_ADD_CIPHER_KEY_CMD layout (from wmi.h, packed struct, all uint8_t):
 *   [0] keyIndex   [1] keyType   [2] keyUsage   [3] keyLength   [4..] key[]
 * keyUsage: PAIRWISE_USAGE=0 (PTK), GROUP_USAGE=1 (GTK)
 * SUPPL_STATUS: 0=FAIL, non-zero=TRUE (SUPPL_STATUS_SUCCESS=1)
 */
void __real_hs_compl_evt(void *ctxt, uint8_t *peer, void *key, int status);

/* Forward declarations — defined later in this file */
static void qcom_ent_open_eap_tx(const uint8_t *bssid);
static void qcom_ent_open_data_tx(const uint8_t *bssid);

void __wrap_hs_compl_evt(void *ctxt, uint8_t *peer, void *key, int status)
{
	const uint8_t *k = (const uint8_t *)key;
	int key_usage = k ? (int)k[2] : -1;

	if (key_usage == HS_KEY_USAGE_PTK) {
		/*
		 * PTK phase: __real_hs_compl_evt (called below) will add the AES
		 * DPM entry with the correct bss_sta_idx from dev->halBssInfo.
		 * Set hs_compl_ptk_ran so pmk_4way_timer_fn knows not to add a
		 * duplicate entry with hal_sta_idx=0.
		 */
		g_ent_ctx.hs_compl_ptk_ran = true;
	}

	if (status != 0 && key_usage == HS_KEY_USAGE_GTK) {
		/*
		 * GTK phase complete: firmware PTK phase (above) already added
		 * the AES DPM entry with correct bss_sta_idx.  Cancel the
		 * fallback timer so it does not fire and check hs_compl_ptk_ran.
		 */
		k_work_cancel_delayable(&g_ent_ctx.pmk_4way_timer);
	}

	__real_hs_compl_evt(ctxt, peer, key, status);
}

/*
 * pmk_4way_timer_fn - fallback timer handler.
 *
 * Fires 600 ms after PMK delivery if data_sta_added is still false.
 *
 * Normal path: __wrap_hs_compl_evt cancels this timer at GTK phase, then
 * __real_hs_compl_evt sends FOURWAY_HANDSHAKE_SUCCESS → qcom_ent_4way_hs_done()
 * sets data_sta_added=true.  Timer never fires in the normal path.
 *
 * Fallback path: timer fires if FOURWAY_HANDSHAKE_SUCCESS did not arrive
 * (firmware hs_compl_evt failed, or the WMI event was dropped by a full queue).
 * In this case hs_compl_evt may or may not have run:
 *   - If it ran:  firmware already added AES DPM staid[0] with correct
 *     bss_sta_idx.  qcom_ent_open_data_tx() will add staid[1] with
 *     hal_sta_idx=0 (best-effort fallback; may cause DXE timeout, but
 *     FOURWAY_HANDSHAKE_SUCCESS was already dropped so we have no better option).
 *   - If it didn't run: no AES DPM entry exists yet.  qcom_ent_open_data_tx()
 *     adds it with hal_sta_idx=0; data TX may work if slot 0 is correct.
 *
 * This runs in the system work queue (cooperative with Zephyr net tasks).
 */
static void pmk_4way_timer_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!g_ent_ctx.data_sta_added) {
		if (!g_ent_ctx.hs_compl_ptk_ran) {
			/*
			 * __wrap_hs_compl_evt was never called for the PTK phase
			 * (intra-archive --wrap miss, or firmware 4WHS did not run).
			 * Add AES DPM entry with hal_sta_idx=0 as best-effort fallback.
			 */
			LOG_WRN("pmk_4way_timer: FOURWAY_HANDSHAKE_SUCCESS not received in 600ms "
				"— hs_compl PTK phase not observed, adding AES DPM entry (hal_sta_idx=0)");
			qcom_ent_open_data_tx(g_ent_ctx.bssid);
			g_ent_ctx.timer_added_fallback_staid = true;
		} else {
			/*
			 * __wrap_hs_compl_evt confirmed the PTK phase ran: firmware's
			 * hs_compl_evt() already added the AES DPM entry with the
			 * correct bss_sta_idx from halBssInfo.  Skip the duplicate.
			 * The FOURWAY_HANDSHAKE_SUCCESS WMI event was dropped (likely
			 * WMI queue contention during EAP-TLS RSA operations).
			 */
			LOG_WRN("pmk_4way_timer: FOURWAY_HANDSHAKE_SUCCESS WMI event dropped "
				"— firmware DPM entry exists (hs_compl PTK ran), raising connect directly");
			g_ent_ctx.data_sta_added = true;
		}

		if (g_ent_ctx.iface) {
			g_ent_ctx.connect_raised = true;
			wifi_mgmt_raise_connect_result_event(g_ent_ctx.iface,
							     WIFI_STATUS_CONN_SUCCESS);
#if defined(CONFIG_WIFI_QCOM_AUTO_DHCPV4)
			/* Use restart (stop+start) rather than start: driver_zephyr.c
			 * already called net_dhcpv4_restart() at EAP auth completion,
			 * leaving DHCP in SELECTING state.  net_dhcpv4_start() is a
			 * no-op when DHCP is not DISABLED, so we must restart to force
			 * a fresh DISCOVER now that the AES data path is ready.
			 */
			net_dhcpv4_restart(g_ent_ctx.iface);
#endif
		} else {
			LOG_ERR("pmk_4way_timer: iface is NULL — cannot raise connect event");
		}
	}
}

/* ---------- EAP TX path helpers ----------
 *
 * Problem: nt_dpm_process_eth_packet_from_stack() needs an inUse=1 DPM STA
 * entry to route EAPOL frames over-the-air.  Normally nt_dpm_add_sta() is
 * called from hs_compl_evt() only after the firmware 4-way handshake — but
 * the 4-way HS needs the PMK which wpa_supplicant derives from EAP auth, and
 * EAP auth needs outbound EAPOL frames to reach the AP.  Circular dependency.
 *
 * Fix: after firmware association, register a temporary DPM STA entry with
 * sec_mode=ENC_NONE so nt_dpm_find_sta_entry_for_eth_pkt() can match it and
 * route EAPOL TX.  ENC_NONE means nt_dpm_tx_header_translation() sets WEP=0
 * and dpuNE=ENCRYPTION_DISABLED — correct for the EAP auth phase.
 *
 * The temporary entry is deleted in qcom_supp_set_key(KEY_FLAG_PMK) before
 * the PMK is delivered to firmware, so hs_compl_evt() can then call
 * nt_dpm_add_sta() again with the real sec_mode (AES/TKIP).
 */

/* qcom_ent_dpm_add_sta - populate a sta_config_t and call nt_dpm_add_sta().
 * Returns 0 on success, non-zero on error.
 */
static int qcom_ent_dpm_add_sta(const uint8_t *bssid, uint8_t sec_mode, uint8_t *staid_out)
{
    qcom_ent_sta_cfg_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.bssid, bssid, sizeof(cfg.bssid));
    cfg.IsAP = 1;
    memcpy(cfg.sta_mac_address, bssid, sizeof(cfg.sta_mac_address));
    cfg.qos_sta  = 1;
    cfg.sec_mode = sec_mode;
    cfg.ht       = 1;
    return nt_dpm_add_sta(&cfg, staid_out, 0 /* hal_sta_idx */);
}

/*
 * qcom_ent_open_eap_tx - add temporary ENC_NONE DPM STA entry.
 * Called from qcom_ent_assoc_event() once the BSSID is known.
 */
static void qcom_ent_open_eap_tx(const uint8_t *bssid)
{
    if (g_ent_ctx.early_sta_added) {
        return;
    }

    int err = qcom_ent_dpm_add_sta(bssid, QCOM_ENT_ENC_NONE, &g_ent_ctx.early_staid);

    if (err == 0) {
        g_ent_ctx.early_sta_added = true;
        LOG_DBG("open_eap_tx: ENC_NONE DPM STA entry added staid=%u", g_ent_ctx.early_staid);
    } else {
        LOG_ERR("open_eap_tx: nt_dpm_add_sta failed err=%d", err);
    }
}

/*
 * qcom_ent_close_eap_tx - delete the temporary ENC_NONE DPM STA entry.
 * Called before PMK delivery (set_key KEY_FLAG_PMK) and from deinit.
 */
static void qcom_ent_close_eap_tx(void)
{
    int err;

    if (!g_ent_ctx.early_sta_added) {
        return;
    }

    err = nt_dpm_delete_sta(g_ent_ctx.early_staid);
    if (err == 0) {
        LOG_DBG("close_eap_tx: ENC_NONE DPM STA entry deleted staid=%u",
                g_ent_ctx.early_staid);
    } else {
        LOG_WRN("close_eap_tx: nt_dpm_delete_sta staid=%u err=%d",
                g_ent_ctx.early_staid, err);
    }
    g_ent_ctx.early_sta_added = false;
    g_ent_ctx.early_staid = 0;
}

/*
 * qcom_ent_open_data_tx - add AES DPM STA entry for encrypted data path.
 *
 * Called from pmk_4way_timer_fn as a last-resort fallback when both:
 *   1. FOURWAY_HANDSHAKE_SUCCESS was not received within 600ms, AND
 *   2. hs_compl_ptk_ran is false (firmware PTK phase was NOT intercepted by
 *      __wrap_hs_compl_evt, so we cannot confirm firmware added a DPM entry).
 *
 * When hs_compl_ptk_ran is true (firmware PTK phase ran), pmk_4way_timer_fn
 * skips this call entirely — firmware already added the correct entry with
 * bss_sta_idx from halBssInfo.  Adding another entry with hal_sta_idx=0 here
 * would create a duplicate pointing at the wrong hardware key slot.
 *
 * hal_sta_idx=0: for QCC730 STA mode this is typically the correct hardware
 * key slot assigned during association, but it is not guaranteed.
 */
static void qcom_ent_open_data_tx(const uint8_t *bssid)
{
    if (g_ent_ctx.data_sta_added) {
        return;
    }

    int err = qcom_ent_dpm_add_sta(bssid, QCOM_ENT_ENC_AES, &g_ent_ctx.data_staid);

    if (err == 0) {
        g_ent_ctx.data_sta_added = true;
        LOG_DBG("open_data_tx: AES DPM STA entry added staid=%u", g_ent_ctx.data_staid);
    } else {
        LOG_ERR("open_data_tx: nt_dpm_add_sta failed err=%d", err);
    }
}

static void qcom_ent_close_data_tx(void)
{
    int err;

    if (!g_ent_ctx.data_sta_added) {
        return;
    }

    err = nt_dpm_delete_sta(g_ent_ctx.data_staid);
    if (err == 0) {
        LOG_DBG("close_data_tx: AES DPM STA entry deleted staid=%u", g_ent_ctx.data_staid);
    } else {
        LOG_WRN("close_data_tx: nt_dpm_delete_sta staid=%u err=%d",
                g_ent_ctx.data_staid, err);
    }
    g_ent_ctx.data_sta_added = false;
    g_ent_ctx.data_staid = 0;
}

/* ---------- supplicant dev ops ---------- */

/*
 * enterprise_eap_rx - called from nt_dpm_process_eap_frame() for EAP frames
 * (EAPOL type=0x00) received from the AP.
 *
 * Routes the frame to the wpa_supplicant event loop via EVENT_EAPOL_RX so
 * that EAP/TLS processing runs in the wpa_supplicant thread (large stack)
 * rather than in data_path_task (2 KB stack).  wpa_supplicant_event_wrapper
 * deep-copies src and data before returning, so the pointers are safe to use
 * after this function returns.
 *
 * @src_addr   AP BSSID (addr2 from 802.11 MAC header)
 * @eapol_data EAPOL packet starting at version byte
 * @eapol_len  length of EAPOL packet in bytes
 */
static void enterprise_eap_rx(const uint8_t *src_addr, const uint8_t *eapol_data,
			       uint16_t eapol_len)
{
	struct zep_drv_if_ctx *if_ctx;
	union wpa_event_data event;

	if (!g_ent_ctx.supp_drv_if_ctx) {
		LOG_WRN("enterprise_eap_rx: supplicant not initialized");
		return;
	}

	if_ctx = (struct zep_drv_if_ctx *)g_ent_ctx.supp_drv_if_ctx;
	if (!if_ctx->supp_if_ctx) {
		LOG_WRN("enterprise_eap_rx: no wpa_supplicant context");
		return;
	}

	LOG_DBG("EAP RX: src=%02x:%02x:%02x len=%u eapol_type=0x%02x",
		src_addr[0], src_addr[1], src_addr[2], eapol_len,
		eapol_len >= 2 ? eapol_data[1] : 0);

	memset(&event, 0, sizeof(event));
	event.eapol_rx.src = src_addr;
	event.eapol_rx.data = eapol_data;
	event.eapol_rx.data_len = eapol_len;
	event.eapol_rx.encrypted = FRAME_ENCRYPTION_UNKNOWN;
	event.eapol_rx.link_id = -1;

	/*
	 * wpa_supplicant_event_wrapper deep-copies src and data before
	 * enqueuing the event, so these stack/DMA pointers are safe after
	 * we return.  EAP/TLS runs in the wpa_supplicant thread (80 KB stack).
	 */
	wpa_supplicant_event_wrapper(if_ctx->supp_if_ctx, EVENT_EAPOL_RX, &event);
}

static void *qcom_supp_init(void *supp_drv_if_ctx,
			    const char *iface_name,
			    struct zep_wpa_supp_dev_callbk_fns *callbk_fns)
{
	memset(&g_ent_ctx, 0, sizeof(g_ent_ctx));
	g_ent_ctx.supp_drv_if_ctx = supp_drv_if_ctx;

	if (callbk_fns) {
		g_ent_ctx.cb = *callbk_fns;
	}

	if (iface_name) {
		strlcpy(g_ent_ctx.ifname, iface_name, sizeof(g_ent_ctx.ifname));
	}

	k_work_init_delayable(&g_ent_ctx.pmk_4way_timer, pmk_4way_timer_fn);

	nt_dpm_set_eap_enterprise_hook(enterprise_eap_rx);

	return &g_ent_ctx;
}

static void qcom_supp_deinit(void *if_priv)
{
	struct k_work_sync sync;

	ARG_UNUSED(if_priv);
	/* Use _sync to wait for any in-flight timer handler to complete
	 * before zeroing the context — k_work_cancel_delayable() does not wait. */
	k_work_cancel_delayable_sync(&g_ent_ctx.pmk_4way_timer, &sync);
	qcom_ent_close_eap_tx();   /* clean up if EAP auth was interrupted */
	qcom_ent_close_data_tx();  /* clean up if data path STA entry was added */
	nt_dpm_set_eap_enterprise_hook(NULL);
	memset(&g_ent_ctx, 0, sizeof(g_ent_ctx));
}

static int qcom_supp_get_capa(void *if_priv, struct wpa_driver_capa *capa)
{
	ARG_UNUSED(if_priv);

	if (!capa) {
		return -EINVAL;
	}

	memset(capa, 0, sizeof(*capa));

	/*
	 * Keep non-SME model for QCC fullmac integration.
	 * Do not set WPA_DRIVER_FLAGS_SME.
	 *
	 * WPA_DRIVER_FLAGS_4WAY_HANDSHAKE_8021X: tells wpa_supplicant that the
	 * driver (firmware) handles the RSN 4-way handshake internally and that
	 * after EAP auth it must deliver the PMK to the driver via
	 * set_key(KEY_FLAG_PMK) rather than running its own 4-way HS.
	 */
	capa->flags |= WPA_DRIVER_FLAGS_4WAY_HANDSHAKE_8021X;

	capa->enc |= WPA_DRIVER_CAPA_ENC_TKIP |
		     WPA_DRIVER_CAPA_ENC_CCMP |
		     WPA_DRIVER_CAPA_ENC_GCMP |
		     WPA_DRIVER_CAPA_ENC_CCMP_256 |
		     WPA_DRIVER_CAPA_ENC_GCMP_256;

	return 0;
}

static int qcom_supp_stub_ret_notsup(void)
{
	return -ENOTSUP;
}

static int qcom_supp_scan2(void *if_priv, struct wpa_driver_scan_params *params)
{
	/* QCC730 fullmac: firmware owns all 802.11 scan/connect.
	 * Return 0 (scan accepted) but never post scan results — the
	 * supplicant waits indefinitely and does not drive its own
	 * reconnect loop.  This is the correct behaviour for a fullmac
	 * driver where the host supplicant handles only EAP. */
	ARG_UNUSED(if_priv);
	ARG_UNUSED(params);
	return 0;
}

static int qcom_supp_scan_abort(void *if_priv)
{
	ARG_UNUSED(if_priv);
	return 0;
}

static int qcom_supp_get_scan_results2(void *if_priv)
{
	/* QCC730 fullmac: firmware owns scanning.
	 *
	 * wpa_drv_zep_get_scan_results2() (driver_zephyr.c) blocks on
	 * drv_resp_sem after calling this op, waiting for scan results.
	 * Signal completion by giving the semaphore directly.
	 *
	 * DO NOT call scan_done() here.  scan_done() (wpa_drv_zep_event_proc_scan_done)
	 * gives drv_resp_sem AND enqueues EVENT_SCAN_RESULTS.  Since this function is
	 * called from within the EVENT_SCAN_RESULTS processing chain:
	 *
	 *   event_socket_handler → wpa_supplicant_event(EVENT_SCAN_RESULTS)
	 *     → wpa_supplicant_get_scan_results → wpa_drv_zep_get_scan_results2
	 *       → this function → scan_done → enqueues EVENT_SCAN_RESULTS #2
	 *
	 * event_socket_handler drains the FIFO in a do-while loop, so EVENT_SCAN_RESULTS #2
	 * is processed immediately after the current event, which calls this function again,
	 * which enqueues EVENT_SCAN_RESULTS #3, and so on — infinite loop.
	 *
	 * Each iteration does alloc + free of scan_res2 (16 bytes) plus two heap
	 * allocs in send_data() for the event message.  After enough iterations the
	 * Zephyr sys_heap free-list becomes corrupted (circular FREE_NEXT pointer),
	 * causing alloc_chunk() to loop forever with interrupts disabled → system freeze.
	 *
	 * Fix: give drv_resp_sem directly.  qcom_supp_scan2() never registers a scan
	 * timeout via eloop_register_timeout(), so there is nothing to cancel.
	 */
	ARG_UNUSED(if_priv);

	if (g_ent_ctx.supp_drv_if_ctx) {
		struct zep_drv_if_ctx *if_ctx =
			(struct zep_drv_if_ctx *)g_ent_ctx.supp_drv_if_ctx;

		if_ctx->scan_res2_get_in_prog = false;
		k_sem_give(&if_ctx->drv_resp_sem);
	}

	return 0;
}

static int qcom_supp_deauthenticate(void *if_priv, const char *addr, unsigned short reason_code)
{
	ARG_UNUSED(if_priv);
	ARG_UNUSED(addr);
	ARG_UNUSED(reason_code);
	return qcom_supp_stub_ret_notsup();
}

static int qcom_supp_authenticate(void *if_priv,
				  struct wpa_driver_auth_params *params,
				  struct wpa_bss *curr_bss)
{
	ARG_UNUSED(if_priv);
	ARG_UNUSED(params);
	ARG_UNUSED(curr_bss);
	return qcom_supp_stub_ret_notsup();
}

static int qcom_supp_associate(void *if_priv, struct wpa_driver_associate_params *params)
{
	ARG_UNUSED(if_priv);
	ARG_UNUSED(params);
	return qcom_supp_stub_ret_notsup();
}

static int qcom_supp_set_key(void *if_priv, const unsigned char *ifname, enum wpa_alg alg,
			     const unsigned char *addr, int key_idx, int set_tx,
			     const unsigned char *seq, size_t seq_len, const unsigned char *key,
			     size_t key_len, enum key_flag key_flag)
{
	ARG_UNUSED(if_priv);
	ARG_UNUSED(ifname);
	ARG_UNUSED(alg);
	ARG_UNUSED(addr);
	ARG_UNUSED(key_idx);
	ARG_UNUSED(set_tx);
	ARG_UNUSED(seq);
	ARG_UNUSED(seq_len);

	if (key_flag & KEY_FLAG_PMK) {
		/*
		 * wpa_supplicant_eapol_cb() delivers the EAP-derived PMK here
		 * (because WPA_DRIVER_FLAGS_4WAY_HANDSHAKE_8021X is set).
		 *
		 * Delete the temporary ENC_NONE DPM STA entry before delivering
		 * the PMK to firmware.  hs_compl_evt() will add a new entry
		 * with the correct sec_mode (AES/TKIP) after the 4-way HS.
		 */
		qcom_ent_close_eap_tx();

		qapi_WLAN_Set_Param(g_ent_ctx.device_id,
				    __QAPI_WLAN_PARAM_GROUP_WIRELESS_SECURITY,
				    __QAPI_WLAN_PARAM_GROUP_SECURITY_PMK,
				    (void *)key, (uint32_t)key_len, false);

		/*
		 * Start the AES DPM fallback timer.
		 * M1→M2→M3→M4 typically completes in <200ms.  Allow 600ms to
		 * accommodate RADIUS latency and AP retry delays (e.g. after
		 * multiple assoc rejections).  FOURWAY_HANDSHAKE_SUCCESS should
		 * arrive via station_connect_event() → qcom_ent_4way_hs_done().
		 * If not (queue-full edge case), the timer raises the connect event
		 * and starts DHCP directly (firmware already added the AES DPM entry
		 * in hs_compl_evt PTK phase).
		 */
		k_work_schedule(&g_ent_ctx.pmk_4way_timer, K_MSEC(600));
		return 0;
	}

	/* All other set_key calls (pairwise/group keys after 4-way HS) are
	 * installed by firmware — return 0 to keep the supplicant happy. */
	ARG_UNUSED(key);
	ARG_UNUSED(key_len);
	ARG_UNUSED(key_flag);
	return 0;
}

/*
 * Called by driver_zephyr.c when the supplicant port authorization state
 * changes (authorized=1 after EAP completes, authorized=0 on deauth).
 *
 * For QCC730 fullmac, EAP auth completing does NOT mean the link is ready —
 * the firmware still needs to complete the 4-way handshake and install keys.
 * The actual connect event is raised by qcom_ent_4way_hs_done() when the
 * firmware sends FOURWAY_HANDSHAKE_SUCCESS.  Log only here.
 */
static int qcom_supp_set_supp_port(void *if_priv, int authorized, char *bssid)
{
	ARG_UNUSED(if_priv);
	ARG_UNUSED(bssid);

	return 0;
}

static int qcom_supp_signal_poll(void *if_priv, struct wpa_signal_info *si, unsigned char *bssid)
{
	ARG_UNUSED(if_priv);
	ARG_UNUSED(si);
	ARG_UNUSED(bssid);
	return qcom_supp_stub_ret_notsup();
}

static int qcom_supp_send_mlme(void *if_priv, const u8 *data, size_t data_len, int noack,
			       unsigned int freq, int no_cck, int offchanok,
			       unsigned int wait_time, int cookie)
{
	ARG_UNUSED(if_priv);
	ARG_UNUSED(data);
	ARG_UNUSED(data_len);
	ARG_UNUSED(noack);
	ARG_UNUSED(freq);
	ARG_UNUSED(no_cck);
	ARG_UNUSED(offchanok);
	ARG_UNUSED(wait_time);
	ARG_UNUSED(cookie);
	return qcom_supp_stub_ret_notsup();
}

static int qcom_supp_get_wiphy(void *if_priv)
{
	ARG_UNUSED(if_priv);
	return qcom_supp_stub_ret_notsup();
}

static int qcom_supp_register_frame(void *if_priv, u16 type, const u8 *match, size_t match_len,
				    bool multicast)
{
	ARG_UNUSED(if_priv);
	ARG_UNUSED(type);
	ARG_UNUSED(match);
	ARG_UNUSED(match_len);
	ARG_UNUSED(multicast);
	return 0;
}

static int qcom_supp_get_conn_info(void *if_priv, struct wpa_conn_info *info)
{
	ARG_UNUSED(if_priv);
	ARG_UNUSED(info);
	return qcom_supp_stub_ret_notsup();
}

static int qcom_supp_set_country(void *if_priv, const char *alpha2)
{
	ARG_UNUSED(if_priv);
	ARG_UNUSED(alpha2);
	return qcom_supp_stub_ret_notsup();
}

static int qcom_supp_get_country(void *if_priv, char *alpha2)
{
	ARG_UNUSED(if_priv);
	ARG_UNUSED(alpha2);
	return qcom_supp_stub_ret_notsup();
}

const struct zep_wpa_supp_dev_ops qcom_wifi_ent_drv_ops = {
	.init = qcom_supp_init,
	.deinit = qcom_supp_deinit,
	.scan2 = qcom_supp_scan2,
	.scan_abort = qcom_supp_scan_abort,
	.get_scan_results2 = qcom_supp_get_scan_results2,
	.deauthenticate = qcom_supp_deauthenticate,
	.authenticate = qcom_supp_authenticate,
	.associate = qcom_supp_associate,
	.set_key = qcom_supp_set_key,
	.set_supp_port = qcom_supp_set_supp_port,
	.signal_poll = qcom_supp_signal_poll,
	.send_mlme = qcom_supp_send_mlme,
	.get_wiphy = qcom_supp_get_wiphy,
	.register_frame = qcom_supp_register_frame,
	.get_capa = qcom_supp_get_capa,
	.get_conn_info = qcom_supp_get_conn_info,
	.set_country = qcom_supp_set_country,
	.get_country = qcom_supp_get_country,
};

/* ---------- called from qwifi_drv_connect() / station_connect_event() ---------- */

/*
 * qcom_ent_assoc_event - post EVENT_ASSOC to wpa_supplicant after the QCC730
 * firmware completes 802.11 authentication and association.
 *
 * This bypasses the supplicant-driven scan/authenticate/associate flow.
 * The supplicant's wpa_supplicant_event_assoc() handler will:
 *   1. Match the BSSID to the pre-configured EAP network via select_config()
 *   2. Set key_mgmt = WPA_KEY_MGMT_IEEE8021X
 *   3. Enable the EAPOL state machine (eapol_sm_notify_portEnabled)
 * EAP frames are received via the enterprise_eap_rx hook registered in qcom_supp_init().
 */
void qcom_ent_assoc_event(const struct device *dev, const uint8_t *bssid, bool success,
			  uint8_t device_id)
{
	union wpa_event_data event;

	if (!g_ent_ctx.supp_drv_if_ctx) {
		LOG_ERR("assoc_event: enterprise glue not initialized");
		return;
	}

	/* Remember device and QAPI device ID for PMK delivery in set_key() */
	g_ent_ctx.dev = dev;
	g_ent_ctx.device_id = device_id;
	g_ent_ctx.iface = net_if_lookup_by_dev(dev);

	memset(&event, 0, sizeof(event));
	memcpy(g_ent_ctx.bssid, bssid, sizeof(g_ent_ctx.bssid));

	/*
	 * Reset per-association state that may carry stale values from a
	 * previous connect attempt.  Without this, data_sta_added=true left
	 * from a prior fallback prevents the fallback timer from adding the
	 * AES DPM entry on subsequent reconnects → DHCP hal_ret=17.
	 * Also clean up any lingering AES DPM entry from the prior session.
	 */
	k_work_cancel_delayable(&g_ent_ctx.pmk_4way_timer);
	qcom_ent_close_data_tx();
	g_ent_ctx.hs_compl_ptk_ran = false;
	g_ent_ctx.connect_raised   = false;
	g_ent_ctx.timer_added_fallback_staid = false;

	/*
	 * Open the temporary ENC_NONE DPM STA entry so outbound EAPOL frames
	 * can be routed over-the-air during EAP authentication.
	 * See qcom_ent_open_eap_tx() for the full rationale.
	 */
	qcom_ent_open_eap_tx(bssid);

	/* Populate ssid in if_ctx so wpa_supplicant_get_ssid() / select_config()
	 * finds the configured network when EVENT_ASSOC is delivered.  Normally
	 * this is set in wpa_drv_zep_authenticate(), which we bypass in the
	 * QCC730 fullmac enterprise path. */
	struct zep_drv_if_ctx *if_ctx = (struct zep_drv_if_ctx *)g_ent_ctx.supp_drv_if_ctx;

	if_ctx->ssid_len = g_ent_ctx.ssid_len;
	memcpy(if_ctx->ssid, g_ent_ctx.ssid, g_ent_ctx.ssid_len);

	/* assoc_info.addr is read by wpa_drv_zep_event_proc_assoc_resp()
	 * and copied to if_ctx->bssid which get_bssid() returns. */
	event.assoc_info.addr = g_ent_ctx.bssid;
	event.assoc_info.resp_ies = NULL;
	event.assoc_info.resp_ies_len = 0;
	event.assoc_info.req_ies = NULL;
	event.assoc_info.req_ies_len = 0;

	wpa_drv_zep_event_proc_assoc_resp(
		(struct zep_drv_if_ctx *)g_ent_ctx.supp_drv_if_ctx,
		&event,
		success ? WLAN_STATUS_SUCCESS : WLAN_STATUS_UNSPECIFIED_FAILURE);
}

/*
 * qcom_ent_4way_hs_done - called from station_connect_event() when the
 * firmware reports FOURWAY_HANDSHAKE_SUCCESS for an EAP security type.
 *
 * At this point the firmware has completed the RSN 4-way handshake, installed
 * the PTK/GTK, and the data path is fully ready.  Raise the Zephyr connect
 * result event and kick off DHCP so upper layers know the link is on-line.
 */
void qcom_ent_4way_hs_done(struct net_if *iface)
{
	/* Cancel the fallback timer — normal completion path took over */
	k_work_cancel_delayable(&g_ent_ctx.pmk_4way_timer);

	/*
	 * Add the AES DPM STA entry for the encrypted data path.
	 *
	 * Root cause analysis (Build 47 log): firmware's hs_compl_evt() is an
	 * intra-archive call — the linker --wrap=hs_compl_evt is never invoked,
	 * so __wrap_hs_compl_evt does not run, hs_compl_ptk_ran stays false.
	 * More importantly, zero "DPM Add STA" events are observed in the log
	 * for the FOURWAY_HANDSHAKE_SUCCESS path, confirming that hs_compl_evt
	 * does NOT call nt_dpm_add_sta() in this firmware/config.  Without an
	 * explicit call here, the DPM table has no AES entry → DHCP TX returns
	 * hal_ret=17 (no DPM entry found).
	 *
	 * Calling open_data_tx() is safe: since hs_compl_evt does not add a
	 * DPM entry, there is no duplicate (the Build 18 staid[0]+staid[1]
	 * issue does not apply here).
	 */
	/*
	 * Dual-staid cleanup: if the fallback timer fired before M1 arrived
	 * (timer_added_fallback_staid=true) and firmware PTK subsequently ran
	 * (hs_compl_ptk_ran=true), the DPM table has two AES entries:
	 *   - data_staid (e.g. staid[0]): timer fallback, hal_sta_idx=0
	 *   - staid[N]: firmware PTK, correct bss_sta_idx from halBssInfo
	 *
	 * nt_dpm_find_sta_entry_for_eth_pkt() scans from i=0 and returns the
	 * lower staid for TX, while BA sessions use conn->station_id = staid[N].
	 * nt_dpm_reorder_queue_flush() deadlocks on the reorder mutex because
	 * the TX path and RX BA path operate on different staid slots →
	 * permanent BA del/add loop → system hang.
	 *
	 * Remove the stale timer entry.  Firmware's staid[N] remains.
	 * Set data_sta_added=true afterward to prevent qcom_ent_open_data_tx()
	 * from adding yet another entry (firmware's is already correct).
	 */
	if (g_ent_ctx.timer_added_fallback_staid && g_ent_ctx.hs_compl_ptk_ran) {
		/*
		 * Timer fired before FOURWAY_HANDSHAKE_SUCCESS AND firmware PTK ran:
		 * DPM table has two AES entries (timer staid + firmware staid).
		 * Remove the stale timer entry; firmware's entry with correct
		 * bss_sta_idx stays.
		 */
		LOG_WRN("4way_hs_done: removing stale timer-fallback DPM entry staid=%u"
			" (firmware PTK added correct entry — dual-staid cleanup)",
			g_ent_ctx.data_staid);
		qcom_ent_close_data_tx();         /* deletes stale staid, clears data_sta_added */
		g_ent_ctx.data_sta_added = true;  /* firmware's staid is live; prevent re-add */
		g_ent_ctx.timer_added_fallback_staid = false;
	} else if (g_ent_ctx.hs_compl_ptk_ran) {
		/*
		 * Normal path: firmware PTK phase ran via __real_hs_compl_evt and
		 * already called nt_dpm_add_sta() with the correct bss_sta_idx from
		 * halBssInfo.  Do NOT add a second entry with hal_sta_idx=0 here —
		 * that creates dual-staid (staid[N] firmware + staid[M] ours) which
		 * causes TX to use the lower staid while BA sessions reference the
		 * higher, producing reorder mutex deadlock → DHCP TX failure / hang.
		 */
		g_ent_ctx.data_sta_added = true;  /* firmware's staid is live; mark as done */
	} else {
		/*
		 * Fallback: __wrap_hs_compl_evt was never called (intra-archive
		 * --wrap miss) so firmware did not add a DPM entry via hs_compl_evt
		 * PTK phase.  Add the AES entry here with hal_sta_idx=0.
		 */
		LOG_INF("4way_hs_done: FOURWAY_HANDSHAKE_SUCCESS received — adding AES DPM entry");
		qcom_ent_open_data_tx(g_ent_ctx.bssid);
	}

	/*
	 * Guard against duplicate Connected event.
	 *
	 * Race: pmk_4way_timer fires (M1 delayed >600ms after PMK delivery) →
	 * timer raises Connected and starts DHCP.  Then the real 4-way HS
	 * completes and FOURWAY_HANDSHAKE_SUCCESS arrives here.  Without this
	 * guard, a second Connected + net_dhcpv4_restart() would launch a
	 * concurrent DHCP session → BA del/add storm → system hang.
	 */
	if (g_ent_ctx.connect_raised) {
		LOG_WRN("4way_hs_done: connect already raised (pmk_4way_timer fired first)"
			" — skipping duplicate Connected and DHCP restart");
		return;
	}
	g_ent_ctx.connect_raised = true;
	wifi_mgmt_raise_connect_result_event(iface, WIFI_STATUS_CONN_SUCCESS);

#if defined(CONFIG_WIFI_QCOM_AUTO_DHCPV4)
	/*
	 * Do NOT call net_dhcpv4_restart() here.
	 *
	 * wpa_drv_zep_set_supp_port(authorized=1) in driver_zephyr.c already
	 * called net_dhcpv4_restart() from the wpa_supplicant thread when EAP
	 * auth completed.  That session is in SELECTING or INIT state and will
	 * retry the DISCOVER automatically (via the DHCP timeout_work) once the
	 * AES DPM entry is available (installed by __real_hs_compl_evt).
	 *
	 * Calling net_dhcpv4_restart() here (even via deferred work) would:
	 *   1. Call net_dhcpv4_stop() which acquires the DHCP mutex.  If the
	 *      net_mgmt event thread currently holds that mutex (processing an
	 *      earlier DHCP event), the system work queue blocks forever and
	 *      the shell becomes unresponsive.
	 *   2. Reset the DHCP state machine unnecessarily, adding ~4s of
	 *      DISCOVER retry delay.
	 */
#endif
}
/*
 * qcom_ent_setup_supplicant - configure wpa_supplicant EAP credentials.
 *
 * Called from qwifi_drv_connect() before qapi_WLAN_Commit().
 * Uses supplicant_config_enterprise_network() which runs wpa_cli commands
 * to set EAP method, certs, identity — but does NOT call select_network,
 * so the supplicant does not trigger its own scan/connect flow.
 */
int qcom_ent_setup_supplicant(const struct device *dev,
			      struct wifi_connect_req_params *params)
{
	/* Save SSID so qcom_ent_assoc_event() can populate if_ctx->ssid,
	 * which wpa_supplicant_get_ssid() / select_config() needs to match
	 * the configured network when EVENT_ASSOC is delivered. */
	g_ent_ctx.ssid_len = (uint8_t)MIN((int)params->ssid_length, (int)SSID_MAX_LEN);
	memcpy(g_ent_ctx.ssid, params->ssid, g_ent_ctx.ssid_len);

	int ret = supplicant_config_enterprise_network(dev, params);

	if (ret) {
		LOG_ERR("supplicant EAP config failed: %d", ret);
	}

	return ret;
}
