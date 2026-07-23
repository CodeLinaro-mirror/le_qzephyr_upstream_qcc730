/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Generic hostap-eloop driver thread for MINIMAL builds.
 *
 * The hostap nan_de / eapol cores assume a single-threaded eloop drives
 * both timeouts and event delivery — that is the model used by
 * supp_main.c when CONFIG_WIFI_NM_WPA_SUPPLICANT_MINIMAL=n. In a MINIMAL
 * profile supp_main is excluded, so eloop_run never gets called and
 * registered timeouts never fire — fatal for nan_de_run_timer / EAPOL
 * retransmission.
 *
 * This file is the missing piece. Mirrors supp_main's design:
 *   - one Zephyr thread runs eloop_run() forever
 *   - a zvfs_eventfd is registered as an eloop "reader" so the loop
 *     never observes an idle empty-set and exits early
 *   - producers (running on other threads, e.g. the WMI dispatch task)
 *     push self-describing messages into a k_fifo and write the eventfd;
 *     the eloop thread wakes, drains the fifo, and invokes each message's
 *     handle() callback in its own context with the glue lock held
 *
 * This layer is feature-agnostic: it has no knowledge of NAN / Enterprise
 * or any specific hostap API. Feature glue defines its own message types
 * (each embedding struct qcom_he_msg) and handlers and calls
 * qcom_hostap_post().
 */

#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/zvfs/eventfd.h>

#include "includes.h"
#include "common.h"
#include "eloop.h"

#include "qcom_hostap_eloop.h"

LOG_MODULE_REGISTER(qcom_hostap_eloop, LOG_LEVEL_INF);

#define QCOM_HE_THREAD_STACK_SIZE 4096
#define QCOM_HE_THREAD_PRIO       K_PRIO_PREEMPT(7)

/* ------------------------------- state ------------------------------- */

static K_FIFO_DEFINE(g_he_fifo);
static K_MUTEX_DEFINE(g_he_lock);
static K_THREAD_STACK_DEFINE(g_he_stack, QCOM_HE_THREAD_STACK_SIZE);
static struct k_thread g_he_tid;
static int    g_he_eventfd = -1;
static bool   g_he_running;
/* Refcount of feature modules (Enterprise, future NAN, ...) that need
 * the hostap eloop thread alive. The thread is started by the first
 * acquirer and torn down by the last releaser. acquire/release are
 * serialised by g_he_lifecycle so the start/stop side never races
 * against itself. */
static atomic_t      g_he_refcnt;
static K_MUTEX_DEFINE(g_he_lifecycle);

/* ----------------------------- helpers ------------------------------- */

void qcom_hostap_lock(void)
{
	k_mutex_lock(&g_he_lock, K_FOREVER);
}

void qcom_hostap_unlock(void)
{
	k_mutex_unlock(&g_he_lock);
}

int qcom_hostap_post(struct qcom_he_msg *msg)
{
	zvfs_eventfd_t one = 1;

	if (!g_he_running) {
		/* Loop hasn't started yet — caller raced with init. Ownership
		 * stays with the caller (it knows how to free its own payload).
		 */
		return -EAGAIN;
	}

	k_fifo_put(&g_he_fifo, msg);
	/* zvfs_eventfd_write is non-blocking with EFD_NONBLOCK; failures here
	 * (semaphore full) only mean the loop hasn't drained yet — the next
	 * read will see all queued items, so we don't need to retry.
	 */
	(void)zvfs_eventfd_write(g_he_eventfd, one);
	return 0;
}

/* ----------------------------- dispatch ------------------------------ */

/* eloop reader: drains the fifo when the eventfd fires. Holds the glue
 * lock around dispatch so concurrent shell-issued calls into hostap don't
 * race with hostap state-machine mutation. Each message carries its own
 * handler, so this loop stays feature-agnostic.
 */
static void event_socket_handler(int sock, void *eloop_ctx, void *user_data)
{
	zvfs_eventfd_t value = 0;
	struct qcom_he_msg *msg;

	(void)eloop_ctx;
	(void)user_data;

	/* Drain the eventfd counter so subsequent writes can wake us again. */
	(void)zvfs_eventfd_read(sock, &value);

	qcom_hostap_lock();
	while ((msg = k_fifo_get(&g_he_fifo, K_NO_WAIT)) != NULL) {
		msg->handle(msg);
	}
	qcom_hostap_unlock();
}

/* ------------------------------ thread ------------------------------- */

static void he_thread(void *a, void *b, void *c)
{
	(void)a; (void)b; (void)c;

	g_he_eventfd = zvfs_eventfd(0, ZVFS_EFD_NONBLOCK);
	if (g_he_eventfd < 0) {
		LOG_ERR("zvfs_eventfd: %d", -errno);
		return;
	}

	/* Registering as a reader keeps eloop.readers.count > 0 so eloop_run's
	 * top-level termination check (no readers && empty timeout list)
	 * doesn't fire and exit the loop. The handler doubles as the message
	 * drain.
	 */
	if (eloop_register_read_sock(g_he_eventfd, event_socket_handler,
				     NULL, NULL) != 0) {
		LOG_ERR("eloop_register_read_sock failed");
		return;
	}

	g_he_running = true;
	LOG_INF("hostap eloop thread running");

	eloop_run();

	g_he_running = false;
	LOG_INF("hostap eloop thread exiting");
}

int qcom_hostap_eloop_acquire(void)
{
	k_tid_t tid;
	int rc = 0;

	/* Refcount under a separate mutex (not g_he_lock!): g_he_lock
	 * is held by the dispatcher across hostap calls, and acquirers
	 * may race in from arbitrary threads (enterprise glue, future
	 * qcom_nan_enable). Using g_he_lock here would needlessly block
	 * the dispatcher.
	 */
	k_mutex_lock(&g_he_lifecycle, K_FOREVER);

	if (atomic_inc(&g_he_refcnt) != 0) {
		/* Someone else already started the thread. */
		goto out;
	}

	/* First acquirer initialises the eloop. MINIMAL does not link
	 * supp_main, so nothing else calls eloop_init() — its static
	 * dl_list head would otherwise stay zeroed and the first
	 * eloop_register/cancel_timeout() would deref NULL. Must run before
	 * the thread (he_thread calls eloop_register_read_sock) and exactly
	 * once (eloop_init memsets the global eloop state). The refcnt 0->1
	 * gate guarantees single execution.
	 */
	if (eloop_init() != 0) {
		atomic_dec(&g_he_refcnt);
		LOG_ERR("eloop_init failed");
		rc = -EIO;
		goto out;
	}

	tid = k_thread_create(&g_he_tid, g_he_stack,
			      K_THREAD_STACK_SIZEOF(g_he_stack),
			      he_thread, NULL, NULL, NULL,
			      QCOM_HE_THREAD_PRIO, 0, K_NO_WAIT);
	if (tid == NULL) {
		eloop_destroy();
		atomic_dec(&g_he_refcnt);
		rc = -ENOMEM;
		goto out;
	}
	k_thread_name_set(tid, "qcom_he");

out:
	k_mutex_unlock(&g_he_lifecycle);
	return rc;
}

void qcom_hostap_eloop_release(void)
{
	k_mutex_lock(&g_he_lifecycle, K_FOREVER);

	/* atomic_dec returns the OLD value; releasing the last reference
	 * brings refcnt 1 -> 0 and is the trigger for tearing down.
	 */
	if (atomic_dec(&g_he_refcnt) != 1) {
		goto out;
	}
	if (!g_he_running) {
		/* Thread crashed or never came up; refcnt was non-zero only
		 * because acquire bumped it before checking. Nothing to tear
		 * down.
		 */
		goto out;
	}
	eloop_terminate();
	if (g_he_eventfd >= 0) {
		zvfs_eventfd_t one = 1;
		(void)zvfs_eventfd_write(g_he_eventfd, one);
	}
	/* Wait for the thread to drain eloop_run and exit so a subsequent
	 * acquire can re-create it cleanly. K_FOREVER is acceptable here:
	 * the only blocker is the eloop reader handler which itself only
	 * holds the glue lock briefly.
	 */
	(void)k_thread_join(&g_he_tid, K_FOREVER);

	/* Reciprocal of eloop_init() above. Safe now that the thread has
	 * joined (no eloop callback can be in flight). */
	eloop_destroy();

out:
	k_mutex_unlock(&g_he_lifecycle);
}
