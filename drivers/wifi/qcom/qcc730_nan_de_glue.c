/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: Apache-2.0
 *
 * QCC730 NAN Discovery Engine glue — MINIMAL hostap build (FR203517).
 *
 * eloop is driven by the shared qcom_hostap_eloop thread (started at WiFi
 * bring-up). nan_glue_init() increments the refcount only — it does NOT
 * call eloop_init() a second time. After nan_de_publish/subscribe() registers
 * the first eloop timeout, qcom_hostap_wakeup() writes the eventfd so the
 * blocked eloop_run() wakes up and processes the new timeout immediately.
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/logging/log.h>

#include "utils/includes.h"
#include "utils/common.h"

#include "utils/eloop.h"
#include "utils/wpabuf.h"
#include "common/nan_de.h"
#include "common/nan.h"

#include "qapi_status.h"
#include <libwifi.h>
#include <libwifi/wmi.h>

extern qapi_Status_t wmi_cmd_send(WMI_COMMAND_ID cmd_id, void *p_data,
				  uint32_t data_len);

#include "inc/qcom_hostap_eloop.h"
#include "qcc730_nan_de_glue.h"

LOG_MODULE_DECLARE(qwifi_drv, CONFIG_WIFI_LOG_LEVEL);

static struct nan_de *g_nan_de;
static uint8_t g_nmi[6];

/* dwell time (ms) requested by nan_de on the most recent TX. nan_de sets
 * de->tx_wait_end_freq after a TX with wait_time != 0 and will NOT advance to
 * the next announce / channel hop until nan_de_tx_wait_ended() is called. The
 * full wpa_supplicant path clears it on an off-channel dwell-ended event; the
 * MINIMAL path has no such event, so we emulate the dwell with an eloop
 * timeout of this duration (see nan_dwell_timeout). Saved in the tx callback,
 * consumed when the matching tx_status arrives. Single service + serial TX, so
 * a single scalar is sufficient. */
static unsigned int g_nan_tx_wait_ms;

/* Two-phase channel management (IDF-3):
 *
 * Phase 1 — Search: publisher rotates across all social channels with ch6
 * weighted higher (4 out of 6 slots) to match Matter commissioner's ch6 bias.
 *
 * Phase 2 — Interaction: once a subscriber is discovered (publisher receives
 * a ssi=NULL follow-up → pauseState, or an active subscribe triggers
 * nan_glue_replied), the discovery channel is recorded as the anchor.
 * Subsequent TX requests for non-anchor channels are silently dropped
 * (returning 0 so nan_de sees success) and only anchor-channel TXes are
 * actually sent to firmware. This keeps the radio focused on the channel
 * where the peer was found, ensuring reliable follow-up exchange.
 *
 * Phase resets to Search when the publish service terminates (TTL / cancel).
 */
/* These variables are written from the eloop thread (nan_de callbacks)
 * and reset from the caller thread (shell) in nan_reset_to_search_phase().
 * On single-core Cortex-M, unsigned int reads/writes are atomic, so no
 * explicit lock is needed for this simple flag-style state. */
static unsigned int g_nan_last_tx_freq = 0;  /* freq of most recent TX    */
static unsigned int g_nan_anchor_freq  = 0;  /* 0 = search phase          */

static void nan_enter_interaction_phase(unsigned int anchor_freq)
{
	if (g_nan_anchor_freq == anchor_freq)
		return;
	g_nan_anchor_freq = anchor_freq;
	LOG_INF("NAN glue: interaction phase anchor=ch%u (%u MHz)",
		anchor_freq == 2412 ? 1   :
		anchor_freq == 2437 ? 6   :
		anchor_freq == 2462 ? 11  :
		anchor_freq == 5180 ? 36  :
		anchor_freq == 5220 ? 44  :
		anchor_freq == 5745 ? 149 : 0,
		anchor_freq);
}

static void nan_reset_to_search_phase(void)
{
	if (g_nan_anchor_freq == 0)
		return;
	g_nan_anchor_freq = 0;
	LOG_INF("NAN glue: search phase (multi-channel)");
}

/* -------------------------------------------------------------------------
 * nan_callbacks — tx
 * ------------------------------------------------------------------------- */

static int nan_glue_tx(void *ctx,
		       unsigned int freq, unsigned int wait_time,
		       const u8 *dst, const u8 *src, const u8 *bssid,
		       const struct wpabuf *buf)
{
	ARG_UNUSED(ctx);

	/* Phase 2 (interaction): only transmit on the anchor channel. Silently
	 * discard TXes to other channels — nan_de sees success (return 0) and
	 * will still advance its state (dwell timer, next announce) normally.
	 * This keeps the radio anchored to the discovery channel for reliable
	 * follow-up exchange until the publish service terminates. */
	if (g_nan_anchor_freq != 0 && freq != g_nan_anchor_freq) {
		LOG_DBG("NAN glue: tx skip freq=%u (anchor=%u)",
			freq, g_nan_anchor_freq);
		/* Pretend success but don't advance dwell (wait_time=0 path). */
		g_nan_last_tx_freq = freq;
		return 0;
	}

	size_t body_len = wpabuf_len(buf);
	size_t cmd_sz   = sizeof(WMI_NAN_SEND_ACTION_CMD) + body_len;

	WMI_NAN_SEND_ACTION_CMD *cmd = k_malloc(cmd_sz);
	if (!cmd)
		return -ENOMEM;

	cmd->freq         = freq;
	cmd->wait_time_ms = wait_time;
	memcpy(cmd->dst_addr, dst,  6);
	memcpy(cmd->src_addr, src,  6);
	memcpy(cmd->bssid,    bssid, 6);
	cmd->buf_len = (uint32_t)body_len;
	if (body_len > 0)
		memcpy(cmd->buf, wpabuf_head(buf), body_len);

	LOG_INF("NAN glue: tx freq=%u dst=" MACSTR " body_len=%u wait=%u",
		freq, MAC2STR(dst), (uint32_t)body_len, wait_time);
	qapi_Status_t ret = wmi_cmd_send(WMI_NAN_SEND_ACTION_CMDID, cmd,
					 (uint32_t)cmd_sz);
	k_free(cmd);
	if (ret != QAPI_OK)
		return -EIO;

	/* Remember the dwell nan_de asked for; the tx_status handler arms an
	 * eloop timeout of this length before calling nan_de_tx_wait_ended(),
	 * so the radio stays on this channel long enough to receive a unicast
	 * SDF reply (follow-up) before nan_de advances / hops channel. */
	g_nan_tx_wait_ms   = wait_time;
	g_nan_last_tx_freq = freq;  /* track for anchor phase detection */
	return 0;
}

/* -------------------------------------------------------------------------
 * nan_callbacks — listen (ROC)
 *
 * Faithfully use the freq passed by nan_de — no channel override.
 * This matches upstream wpa_supplicant design (wpas_nan_de_listen uses
 * lwork->freq = freq directly). For subscribe, nan_de passes srv->freq
 * which comes from nan_subscribe_params.freq (default NAN_USD_DEFAULT_FREQ
 * = 2437). For solicited publish, nan_de passes whatever its multi-channel
 * state machine selected. Keeping the freq intact ensures listen_freq
 * accounting stays correct and the listen loop never breaks.
 * ------------------------------------------------------------------------- */

static int nan_glue_listen(void *ctx, unsigned int freq, unsigned int duration)
{
	ARG_UNUSED(ctx);

	LOG_INF("NAN glue: listen freq=%u duration=%u", freq, duration);
	WMI_NAN_REMAIN_ON_CHANNEL_CMD cmd = {
		.freq        = freq,
		.duration_ms = duration,
	};
	qapi_Status_t ret = wmi_cmd_send(WMI_NAN_REMAIN_ON_CHANNEL_CMDID,
					 &cmd, sizeof(cmd));
	return (ret == QAPI_OK) ? 0 : -EIO;
}

/* -------------------------------------------------------------------------
 * nan_callbacks — NAN DE events
 * ------------------------------------------------------------------------- */

static void nan_glue_discovery_result(void *ctx, int subscribe_id,
				      enum nan_service_protocol_type srv_proto_type,
				      const u8 *ssi, size_t ssi_len,
				      int peer_publish_id,
				      const u8 *peer_addr, bool fsd, bool fsd_gas)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(fsd); ARG_UNUSED(fsd_gas);
	LOG_INF("NAN: discovery_result sub_id=%d proto=%d peer_pub_id=%d "
		"peer=" MACSTR " ssi_len=%u",
		subscribe_id, (int)srv_proto_type, peer_publish_id,
		MAC2STR(peer_addr), (uint32_t)ssi_len);
	if (ssi && ssi_len > 0)
		LOG_HEXDUMP_INF(ssi, ssi_len, "NAN: discovery_result ssi");
}

static void nan_glue_replied(void *ctx, int publish_id,
			     const u8 *peer_addr, int peer_subscribe_id,
			     enum nan_service_protocol_type srv_proto_type,
			     const u8 *ssi, size_t ssi_len)
{
	ARG_UNUSED(ctx); ARG_UNUSED(ssi); ARG_UNUSED(ssi_len);
	LOG_INF("NAN: replied pub_id=%d proto=%d peer_sub_id=%d peer=" MACSTR,
		publish_id, (int)srv_proto_type, peer_subscribe_id,
		MAC2STR(peer_addr));
	/* Active subscriber found us — enter interaction phase anchored to the
	 * channel of the most recent TX (the solicited publish reply). */
	if (g_nan_last_tx_freq != 0)
		nan_enter_interaction_phase(g_nan_last_tx_freq);
}

static void nan_glue_publish_terminated(void *ctx, int publish_id,
					enum nan_de_reason reason)
{
	ARG_UNUSED(ctx);
	LOG_INF("NAN: publish_terminated pub_id=%d reason=%d",
		publish_id, (int)reason);
	/* Publish service ended — reset to search phase so the next publish
	 * starts fresh with multi-channel rotation. */
	nan_reset_to_search_phase();
}

static void nan_glue_subscribe_terminated(void *ctx, int subscribe_id,
					  enum nan_de_reason reason)
{
	ARG_UNUSED(ctx);
	LOG_INF("NAN: subscribe_terminated sub_id=%d reason=%d",
		subscribe_id, (int)reason);
}

static void nan_glue_receive(void *ctx, int id, int peer_instance_id,
			     const u8 *ssi, size_t ssi_len,
			     const u8 *peer_addr)
{
	ARG_UNUSED(ctx);
	LOG_INF("NAN: receive id=%d peer_id=%d peer=" MACSTR " ssi_len=%u",
		id, peer_instance_id, MAC2STR(peer_addr), (uint32_t)ssi_len);
	if (ssi && ssi_len > 0)
		LOG_HEXDUMP_INF(ssi, ssi_len, "NAN: receive ssi");

	/* Passive subscriber auto-reply (ssi=NULL follow-up) means nan_de has
	 * entered pauseState for this publisher. The peer was reachable on the
	 * channel of our last TX — anchor to it for reliable follow-up exchange.
	 * (Active subscriber path is handled in nan_glue_replied.) */
	if ((!ssi || ssi_len == 0) && g_nan_last_tx_freq != 0)
		nan_enter_interaction_phase(g_nan_last_tx_freq);
}

static const struct nan_callbacks g_nan_glue_cbs = {
	.ctx                  = NULL,
	.tx                   = nan_glue_tx,
	.listen               = nan_glue_listen,
	.discovery_result     = nan_glue_discovery_result,
	.replied              = nan_glue_replied,
	.publish_terminated   = nan_glue_publish_terminated,
	.subscribe_terminated = nan_glue_subscribe_terminated,
	.receive              = nan_glue_receive,
};

/* -------------------------------------------------------------------------
 * Lifecycle helpers
 * ------------------------------------------------------------------------- */

static int nan_glue_init(const uint8_t *nmi)
{
	if (g_nan_de) {
		LOG_WRN("NAN glue: already initialised");
		return 0;
	}

	/* eloop was already initialised by qcom_hostap_eloop_acquire() at WiFi
	 * bring-up. Only increment the refcount here — do NOT call eloop_init()
	 * again (that would reset global state and break the running thread).
	 */
	if (qcom_hostap_eloop_acquire() != 0) {
		LOG_ERR("NAN glue: hostap eloop acquire failed");
		return -EIO;
	}

	memcpy(g_nmi, nmi, 6);
	g_nan_de = nan_de_init(g_nmi, false /* ap */, &g_nan_glue_cbs);
	if (!g_nan_de) {
		LOG_ERR("NAN glue: nan_de_init failed");
		qcom_hostap_eloop_release();
		return -ENOMEM;
	}

	LOG_INF("NAN glue: initialised nmi=" MACSTR, MAC2STR(g_nmi));
	return 0;
}

static int ensure_init(void)
{
	if (g_nan_de)
		return 0;

	struct net_if *iface = net_if_get_wifi_sta();

	if (!iface) {
		LOG_ERR("NAN glue: WiFi STA interface not ready");
		return -ENODEV;
	}

	struct net_linkaddr *ll = net_if_get_link_addr(iface);

	if (!ll || ll->len < 6) {
		LOG_ERR("NAN glue: cannot retrieve MAC address");
		return -ENODEV;
	}

	return nan_glue_init(ll->addr);
}

/* -------------------------------------------------------------------------
 * Upper API — called by qnan_shell.c
 * ------------------------------------------------------------------------- */

int qcc730_nan_glue_publish(const char *service_name, uint8_t srv_proto_type,
			    const uint8_t *ssi, size_t ssi_len,
			    unsigned int ttl,
			    bool unsolicited, bool solicited,
			    const int *freq_list)
{
	int ret = ensure_init();

	if (ret)
		return ret;

	/* Reset to search phase only after confirming init succeeded. */
	nan_reset_to_search_phase();

	/* nan_de rejects a publish with both types disabled; default to
	 * unsolicited if the caller cleared both. */
	if (!unsolicited && !solicited)
		unsolicited = true;

	struct wpabuf *ssi_buf = NULL;

	if (ssi && ssi_len > 0) {
		ssi_buf = wpabuf_alloc_copy(ssi, ssi_len);
		if (!ssi_buf)
			return -ENOMEM;
	}

	struct nan_publish_params params = {
		.unsolicited         = unsolicited,
		.solicited           = solicited,
		.ttl                 = ttl,
		.announcement_period = 500,
	};

	/* Default freq_list: 2.4GHz with ch6 weighted 2x (50% of 4 slots).
	 * ch6 gets double weight because the Matter commissioner is hardcoded
	 * to ch6 (2437 MHz), improving discovery latency without sacrificing
	 * ch1/ch11 reachability.
	 *
	 * For 5GHz, caller passes an explicit freq_list with ch149 weighted:
	 *   single ch149:           freq_list=5745
	 *   3-ch rotation weighted: freq_list=5745,5745,5180,5220
	 * ch149 (5745 MHz) is the Matter 5GHz default, analogous to ch6. */
	static int freq_list_2g[] = {2437, 2437, 2412, 2462, 0};

	if (freq_list) {
		params.freq_list = freq_list;
		params.freq = freq_list[0];
	} else {
		params.freq_list = freq_list_2g;
		params.freq = 2437;
	}

	int id = nan_de_publish(g_nan_de, service_name,
				(enum nan_service_protocol_type)srv_proto_type,
				ssi_buf, NULL, &params);
	wpabuf_free(ssi_buf);

	/* Wake the eloop thread so it picks up the newly registered
	 * announce timeout without waiting for the next eventfd write. */
	qcom_hostap_wakeup();

	if (id < 0)
		LOG_ERR("NAN glue: nan_de_publish failed (%d)", id);
	else
		LOG_INF("NAN glue: publish started id=%d svc=%s ttl=%u "
			"unsol=%d sol=%d",
			id, service_name, ttl, unsolicited, solicited);
	return id;
}

int qcc730_nan_glue_subscribe(const char *service_name, uint8_t srv_proto_type,
			      bool active, unsigned int ttl, unsigned int freq)
{
	int ret = ensure_init();

	if (ret)
		return ret;

	/* Use caller-supplied freq; 0 means use default (NAN_USD_DEFAULT_FREQ).
	 * Matches upstream: subscriber listens on a single fixed channel —
	 * publisher's freq_list drives multi-channel rotation on the TX side. */
	if (freq == 0)
		freq = 2437;

	struct nan_subscribe_params params = {
		.active = active,
		/* ttl > 0 keeps the subscribe service alive instead of being
		 * torn down on the first DiscoveryResult (nan_de_srv_expired:
		 * non-FSD subscriber expires immediately once first_discovered
		 * is set). A live subscriber keeps replying with a follow-up
		 * each time it receives the publisher's announce (rx_publish
		 * does not de-dup), so across multiple announce periods the
		 * unicast reply eventually lands within the publisher's dwell
		 * window even on multi-channel. This is the repetition the USD
		 * spec relies on for reliable delivery. */
		.ttl    = ttl,
		.freq   = freq,
	};

	int id = nan_de_subscribe(g_nan_de, service_name,
				  (enum nan_service_protocol_type)srv_proto_type,
				  NULL, NULL, &params);

	/* Wake the eloop thread so it picks up the first listen timeout. */
	qcom_hostap_wakeup();
	if (id < 0)
		LOG_ERR("NAN glue: nan_de_subscribe failed (%d)", id);
	else
		LOG_INF("NAN glue: subscribe started id=%d svc=%s active=%d ttl=%u freq=%u",
			id, service_name, active, ttl, freq);
	return id;
}

void qcc730_nan_glue_cancel_publish(int publish_id)
{
	if (g_nan_de)
		nan_de_cancel_publish(g_nan_de, publish_id);
}

int qcc730_nan_glue_transmit(int handle, const uint8_t *peer_addr,
			     uint8_t req_instance_id,
			     const uint8_t *ssi, size_t ssi_len)
{
	if (!g_nan_de)
		return -ENODEV;

	struct wpabuf *ssi_buf = NULL;

	if (ssi && ssi_len > 0) {
		ssi_buf = wpabuf_alloc_copy(ssi, ssi_len);
		if (!ssi_buf)
			return -ENOMEM;
	}

	int ret = nan_de_transmit(g_nan_de, handle, ssi_buf, NULL,
				  peer_addr, req_instance_id);
	wpabuf_free(ssi_buf);

	/* nan_de_transmit registers a TX via nan_de_tx_sdf -> cb.tx; wake the
	 * eloop so the follow-up dwell / timer is serviced promptly. */
	qcom_hostap_wakeup();

	if (ret < 0)
		LOG_ERR("NAN glue: nan_de_transmit failed (%d)", ret);
	else
		LOG_INF("NAN glue: followup tx handle=%d peer=" MACSTR
			" peer_id=%u ssi_len=%u",
			handle, MAC2STR(peer_addr), req_instance_id,
			(uint32_t)ssi_len);
	return ret;
}

/* -------------------------------------------------------------------------
 * Message types for thread-safe WMI → eloop delivery
 * ------------------------------------------------------------------------- */

struct nan_roc_msg {
	struct qcom_he_msg base;
	uint32_t freq;
	uint8_t  started;
};

struct nan_tx_status_msg {
	struct qcom_he_msg base;
	uint32_t freq;
	uint8_t  dst_addr[6];
};

struct nan_rx_sdf_msg {
	struct qcom_he_msg base;
	uint8_t  src_addr[6];
	uint32_t freq;
	uint32_t body_len;
	uint8_t  body[1]; /* variable length */
};

/* -------------------------------------------------------------------------
 * WMI event entry points — called from qcom_wifi_drv.c (WMI dispatch thread)
 *
 * Do NOT call nan_de_* directly here — that would race with eloop timeout
 * callbacks running in the eloop thread. Instead, post a message; the eloop
 * thread's event_socket_handler drains the fifo and calls the handle()
 * callback safely.
 * ------------------------------------------------------------------------- */

static void nan_roc_handle(struct qcom_he_msg *m)
{
	struct nan_roc_msg *msg = CONTAINER_OF(m, struct nan_roc_msg, base);

	if (g_nan_de) {
		LOG_INF("NAN glue: roc_evt freq=%u started=%d",
			msg->freq, msg->started);
		if (msg->started) {
			nan_de_listen_started(g_nan_de, msg->freq, 0);
		} else {
			/* listen_ended -> nan_de_run_timer() registers the next
			 * listen timeout. Wake the eloop so it re-evaluates and
			 * schedules the next ROC window; without this a solicited
			 * publisher (no announce timer to drive the loop) listens
			 * only once and then goes silent. */
			nan_de_listen_ended(g_nan_de, msg->freq);
			qcom_hostap_wakeup();
		}
	}
	k_free(msg);
}

void qcc730_nan_glue_roc_evt(void *data)
{
	WMI_NAN_REMAIN_ON_CHANNEL_EVT *evt =
		(WMI_NAN_REMAIN_ON_CHANNEL_EVT *)data;
	struct nan_roc_msg *msg = k_malloc(sizeof(*msg));

	if (!msg)
		return;
	msg->base.handle = nan_roc_handle;
	msg->freq    = evt->freq;
	msg->started = evt->started;
	if (qcom_hostap_post(&msg->base) != 0)
		k_free(msg);
}

/* eloop timeout: end of the post-TX dwell window. Runs on the eloop thread.
 * Clearing tx_wait_end_freq lets nan_de resume (next announce / channel hop);
 * the wakeup nudges eloop_run() to re-evaluate immediately. */
static void nan_dwell_timeout(void *eloop_ctx, void *timeout_ctx)
{
	ARG_UNUSED(eloop_ctx);
	ARG_UNUSED(timeout_ctx);

	if (g_nan_de)
		nan_de_tx_wait_ended(g_nan_de);
	qcom_hostap_wakeup();
}

static void nan_tx_status_handle(struct qcom_he_msg *m)
{
	struct nan_tx_status_msg *msg =
		CONTAINER_OF(m, struct nan_tx_status_msg, base);

	if (g_nan_de) {
		nan_de_tx_status(g_nan_de, msg->freq, msg->dst_addr);

		/* Emulate the off-channel dwell: instead of ending the TX wait
		 * immediately (which would let nan_de hop channel before a
		 * unicast SDF reply can arrive), arm an eloop timeout for the
		 * dwell nan_de requested. nan_de holds tx_wait_end_freq until
		 * then, so it neither sends the next announce nor changes
		 * channel, and the radio stays on this channel with RX enabled.
		 * wait==0 means no dwell was requested -> end immediately. */
		if (g_nan_tx_wait_ms > 0) {
			eloop_cancel_timeout(nan_dwell_timeout, NULL, NULL);
			eloop_register_timeout(g_nan_tx_wait_ms / 1000,
					       (g_nan_tx_wait_ms % 1000) * 1000,
					       nan_dwell_timeout, NULL, NULL);
		} else {
			nan_de_tx_wait_ended(g_nan_de);
		}
	}
	k_free(msg);
}

void qcc730_nan_glue_tx_status_evt(void *data)
{
	WMI_NAN_TX_STATUS_EVT *evt = (WMI_NAN_TX_STATUS_EVT *)data;
	struct nan_tx_status_msg *msg = k_malloc(sizeof(*msg));

	if (!msg)
		return;
	msg->base.handle = nan_tx_status_handle;
	msg->freq = evt->freq;
	memcpy(msg->dst_addr, evt->dst_addr, 6);
	if (qcom_hostap_post(&msg->base) != 0)
		k_free(msg);
}

static void nan_rx_sdf_handle(struct qcom_he_msg *m)
{
	struct nan_rx_sdf_msg *msg =
		CONTAINER_OF(m, struct nan_rx_sdf_msg, base);

	if (g_nan_de && msg->body_len > 6) {
		nan_de_rx_sdf(g_nan_de, msg->src_addr, msg->freq,
			      msg->body + 6, msg->body_len - 6);
	}
	k_free(msg);
}

void qcc730_nan_glue_rx_sdf_evt(void *data)
{
	WMI_NAN_RX_SDF_EVT *evt = (WMI_NAN_RX_SDF_EVT *)data;

	if (!g_nan_de || evt->buf_len == 0)
		return;

	struct nan_rx_sdf_msg *msg =
		k_malloc(sizeof(*msg) + evt->buf_len);

	if (!msg)
		return;
	msg->base.handle = nan_rx_sdf_handle;
	memcpy(msg->src_addr, evt->src_addr, 6);
	msg->freq     = evt->freq;
	msg->body_len = evt->buf_len;
	memcpy(msg->body, evt->buf, evt->buf_len);
	if (qcom_hostap_post(&msg->base) != 0)
		k_free(msg);
}
