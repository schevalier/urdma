/*
 * Software iWARP device driver for Linux
 *
 * Authors: Bernard Metzler <bmt@zurich.ibm.com>
 *          Patrick MacArthur <pam@zurich.ibm.com>
 *
 * Copyright (c) 2008-2016, IBM Corporation
 * Copyright (c) 2016, University of New Hampshire
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   - Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   - Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *   - Neither the name of IBM nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _SIW_CM_H
#define _SIW_CM_H

#include <net/sock.h>
#include <linux/tcp.h>

#include <rdma/iw_cm.h>

#include "proto_trp.h"

#define URDMA_PDATA_LEN_MAX 512

enum siw_cep_state {
	SIW_EPSTATE_IDLE = 1,
	SIW_EPSTATE_LISTENING,
	SIW_EPSTATE_CONNECTING,
	SIW_EPSTATE_AWAIT_MPAREQ,
	SIW_EPSTATE_RECVD_MPAREQ,
	SIW_EPSTATE_AWAIT_MPAREP,
	SIW_EPSTATE_RECVD_MPAREP,
	SIW_EPSTATE_ACCEPTING,
	SIW_EPSTATE_RDMA_MODE,
	SIW_EPSTATE_CLOSED
};

struct siw_mpa_info {
	struct trp_rr	hdr;	/* peer mpa hdr in host byte order */
	char		pdata[URDMA_PDATA_LEN_MAX];
				/* private data, plus up to four pad bytes */
	int		bytes_rcvd;
	char		*send_pdata;
	int		send_pdata_size;
};

struct siw_llp_info {
	struct socket		*sock;
	struct sockaddr_in	laddr;	/* redundant with socket info above */
	struct sockaddr_in	raddr;	/* dito, consider removal */
	struct siw_sk_upcalls	sk_def_upcalls;
};

struct siw_dev;

struct siw_cep {
	struct iw_cm_id		*cm_id;
	struct siw_dev		*sdev;

	struct list_head	devq;
	/*
	 * The provider_data element of a listener IWCM ID
	 * refers to a list of one or more listener CEPs
	 */
	struct list_head	listenq;

	/* The list of connections that have been disconnected but we have not
	 * yet notified userspace. */
	struct list_head	disconnect_entry;

	/* The list of connections that have been established but we have not
	 * yet notified userspace. */
	struct list_head	established_entry;

	/* The list of connections that have been established, and userspace
	 * has been notified, but has not yet informed us that they have
	 * finished setup. */
	struct list_head	rtr_wait_entry;

	struct siw_cep		*listen_cep;
	struct siw_qp		*qp;
	spinlock_t		lock;
	wait_queue_head_t	waitq;
	struct kref		ref;
	enum siw_cep_state	state;
	short			in_use;
	struct siw_cm_work	*timer;
	struct list_head	work_freelist;
	struct siw_llp_info	llp;
	struct siw_mpa_info	mpa;
	uint16_t		urdmad_dev_id;
	uint16_t		urdmad_qp_id;
	uint16_t		ord;
	uint16_t		ird;
	int			sk_error; /* not (yet) used XXX */
};

#define MPAREQ_TIMEOUT	(HZ*10)
#define MPAREP_TIMEOUT	(HZ*5)

enum siw_work_type {
	SIW_CM_WORK_ACCEPT	= 1,
	SIW_CM_WORK_READ_MPAHDR,
	SIW_CM_WORK_CLOSE_LLP,		/* close socket */
	SIW_CM_WORK_PEER_CLOSE,		/* socket indicated peer close */
	SIW_CM_WORK_TIMEOUT
};

struct siw_cm_work {
	struct delayed_work	work;
	struct list_head	list;
	enum siw_work_type	type;
	struct siw_cep	*cep;
};

/*
 * With kernel 3.12, OFA ddressing changed from sockaddr_in to
 * sockaddr_storage
 */
#define to_sockaddr_in(a) (*(struct sockaddr_in *)(&(a)))

extern int siw_connect(struct iw_cm_id *, struct iw_cm_conn_param *);
extern int siw_accept(struct iw_cm_id *, struct iw_cm_conn_param *);
extern int siw_reject(struct iw_cm_id *, const void *, u8);
extern int siw_create_listen(struct iw_cm_id *, int);
extern int siw_destroy_listen(struct iw_cm_id *);

extern void siw_cep_get(struct siw_cep *);
extern void siw_cep_put(struct siw_cep *);
extern int siw_qp_rtr_fail(struct siw_cep *);
extern int siw_qp_rtr(struct siw_cep *);
extern int siw_cm_queue_work(struct siw_cep *, enum siw_work_type);

extern int siw_cm_init(void);
extern void siw_cm_exit(void);

/*
 * TCP socket interface
 */
#define sk_to_qp(sk)	(((struct siw_cep *)((sk)->sk_user_data))->qp)
#define sk_to_cep(sk)	((struct siw_cep *)((sk)->sk_user_data))

#endif
