/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef QCOM_HOSTAP_ELOOP_H_
#define QCOM_HOSTAP_ELOOP_H_

/*
 * Generic hostap-eloop thread infrastructure.
 *
 * Hostap's nan_de / eapol cores assume a single-threaded eloop drives both
 * timeouts and event delivery (the supplicant model). MINIMAL builds that
 * exclude supp_main rely on this module to run eloop on a dedicated Zephyr
 * thread started at WiFi interface bring-up.
 *
 * This layer is feature-agnostic: it knows nothing about NAN or any
 * specific hostap API. Producers (running on other threads, e.g. the WMI
 * dispatch task) allocate a message that embeds struct qcom_he_msg as its
 * first member, set the handle callback, and call qcom_hostap_post(). The
 * eloop thread later invokes handle() in its own context with the glue lock
 * held; handle() does the work and frees the message. New features add their
 * own message types and handlers without touching this file.
 */

/* Base of every eloop message. Embed as the FIRST member of a feature-
 * specific message struct so it can be recovered with CONTAINER_OF():
 *
 *   struct my_ev { struct qcom_he_msg msg; ... payload ... };
 *
 * handle() runs on the eloop thread under the glue lock. It is responsible
 * for processing the message and freeing the containing allocation (and any
 * payload it owns). fifo_reserved must stay first for k_fifo.
 */
struct qcom_he_msg {
	void *fifo_reserved;
	void (*handle)(struct qcom_he_msg *self);
};

/* Enqueue a message and wake the eloop thread. On success returns 0 and the
 * eloop thread takes ownership (it will call msg->handle, which frees it).
 * Returns -EAGAIN if the thread is not running yet; in that case ownership
 * stays with the caller, which must free the message itself.
 */
int qcom_hostap_post(struct qcom_he_msg *msg);

/* Reference-counted thread lifecycle. The first acquirer runs eloop_init()
 * and starts the thread; the last releaser terminates it, joins, and runs
 * eloop_destroy(). Both are idempotent and serialised against each other,
 * so multiple features (Enterprise today, future NAN) can each
 * acquire/release safely.
 */
int  qcom_hostap_eloop_acquire(void);
void qcom_hostap_eloop_release(void);

/* Acquire / release the hostap-glue lock around any direct call into hostap
 * APIs from a context other than the eloop thread (e.g. shell commands). The
 * eloop thread takes the lock itself before dispatching queued messages.
 */
void qcom_hostap_lock(void);
void qcom_hostap_unlock(void);

#endif /* QCOM_HOSTAP_ELOOP_H_ */
