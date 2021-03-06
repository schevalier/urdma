/* src/urdmad/main.c */

/*
 * Userspace Software iWARP library for DPDK
 *
 * Author: Patrick MacArthur <patrick@patrickmacarthur.net>
 *
 * Copyright (c) 2016-2017, University of New Hampshire
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
 *   - Neither the names of IBM, UNH, nor the names of its contributors may be
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE

#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <rte_ethdev.h>
#include <rte_errno.h>
#include <rte_ip.h>
#include <rte_kni.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_ring.h>

#include "config_file.h"
#include "interface.h"
#include "list.h"
#include "kni.h"
#include "util.h"
#include "urdmad_private.h"
#include "urdma_kabi.h"


static struct usiw_driver *driver;

static unsigned int core_avail;
static uint32_t core_mask[RTE_MAX_LCORE / 32];

static const unsigned int core_mask_shift = 5;
static const uint32_t core_mask_mask = 31;


static void init_core_mask(void)
{
	struct rte_config *config;
	unsigned int i;

	config = rte_eal_get_configuration();
	for (i = 0; i < RTE_MAX_LCORE; ++i) {
		if (!lcore_config[i].detected) {
			return;
		} else if (config->lcore_role[i] == ROLE_OFF) {
			core_mask[i >> core_mask_shift]
						|= 1 << (i & core_mask_mask);
			core_avail++;
		}
	}
	RTE_LOG(INFO, USER1, "%u cores available\n", core_avail);
} /* init_core_mask */


/** Allocates an array that can be used with reserve_cores().  The caller must
 * call free() when done with this array. */
static uint32_t *alloc_lcore_mask(void)
{
	return malloc(RTE_MAX_LCORE / sizeof(uint32_t));
} /* alloc_lcore_mask */


/** Reserve count lcores for the given process.  Expects out_mask to be a
 * zero-initialized bitmask that can hold RTE_MAX_LCORE bits; i.e., an array
 * with at least (RTE_MAX_LCORE / 32) uint32_t elements.  This can be done with
 * the alloc_lcore_mask() function. */
static bool reserve_cores(unsigned int count, uint32_t *out_mask)
{
	uint32_t bit;
	unsigned int i, j;

	RTE_LOG(DEBUG, USER1, "requesting %u cores; %u cores available\n",
			count, core_avail);
	if (count > core_avail) {
		return false;
	}

	for (i = 0, j = 0; i < count; ++i) {
		while (!core_mask[j]) {
			j++;
			assert(j < RTE_MAX_LCORE / 32);
		}
		bit = 1 << rte_bsf32(core_mask[j]);
		core_mask[j] &= ~bit;
		out_mask[j] |= bit;
	}

	core_avail -= count;
	return true;
} /* reserve_cores */


/** Returns count lcores from the given process.  Expects in_mask to be a
 * bitmask that can hold RTE_MAX_LCORE bits; i.e., an array with at least
 * (RTE_MAX_LCORE / 32) uint32_t elements, where each lcore being returned is
 * set to 1. */
static void return_lcores(uint32_t *in_mask)
{
	uint32_t tmp, bit;
	unsigned int i;

	for (i = 0; i < RTE_MAX_LCORE / (8 * sizeof(*in_mask)); ++i) {
		tmp = in_mask[i];
		while (tmp) {
			core_avail++;
			bit = 1 << rte_bsf32(tmp);
			tmp &= ~bit;
			core_mask[i] |= bit;
		}
	}
} /* return_lcores */


static void
handle_qp_disconnected_event(struct urdma_qp_disconnected_event *event, size_t count)
{
	enum { mbuf_count = 4 };
	struct rte_eth_fdir_filter fdirf;
	struct rte_mbuf *mbuf[mbuf_count];
	struct usiw_port *dev;
	struct urdmad_qp *qp;
	int ret;

	if (count < sizeof(*event)) {
		static bool warned = false;
		if (!warned) {
			RTE_LOG(ERR, USER1, "Read only %zd/%zu bytes\n",
					count, sizeof(*event));
			warned = true;
		}
		return;
	}

	RTE_LOG(DEBUG, USER1, "Got disconnected event for device %" PRIu16 " queue pair %" PRIu16 "\n",
			event->urdmad_dev_id, event->urdmad_qp_id);

	dev = &driver->ports[event->urdmad_dev_id];
	qp = &dev->qp[event->urdmad_qp_id];

	if (dev->flags & port_fdir) {
		memset(&fdirf, 0, sizeof(fdirf));
		fdirf.input.flow_type = RTE_ETH_FLOW_NONFRAG_IPV4_UDP;
		fdirf.input.flow.udp4_flow.ip.dst_ip = dev->ipv4_addr;
		fdirf.action.behavior = RTE_ETH_FDIR_ACCEPT;
		fdirf.action.report_status = RTE_ETH_FDIR_NO_REPORT_STATUS;
		fdirf.soft_id = qp->rx_queue;
		fdirf.action.rx_queue = qp->rx_queue;
		fdirf.input.flow.udp4_flow.dst_port = qp->local_udp_port;
		ret = rte_eth_dev_filter_ctrl(dev->portid,
				RTE_ETH_FILTER_FDIR, RTE_ETH_FILTER_DELETE,
				&fdirf);

		if (ret) {
			RTE_LOG(DEBUG, USER1, "Could not delete fdir filter for qp %" PRIu32 ": %s\n",
					qp->qp_id, rte_strerror(ret));
		}

		/* Drain the queue of any outstanding messages. */
		do {
			ret = rte_eth_rx_burst(dev->portid, qp->rx_queue,
					mbuf, mbuf_count);
		} while (ret > 0);
	}
} /* handle_qp_disconnected_event */

static void
handle_qp_connected_event(struct urdma_qp_connected_event *event, size_t count)
{
	struct urdma_qp_rtr_event rtr_event;
	struct rte_eth_fdir_filter fdirf;
	struct rte_eth_rxq_info rxq_info;
	struct usiw_port *dev;
	struct urdmad_qp *qp;
	ssize_t ret;

	if (count < sizeof(*event)) {
		static bool warned = false;
		if (!warned) {
			RTE_LOG(ERR, USER1, "Read only %zd/%zu bytes\n",
					count, sizeof(*event));
			warned = true;
		}
		return;
	}

	RTE_LOG(DEBUG, USER1, "Got connection event for device %" PRIu16 " queue pair %" PRIu32 "/%" PRIu16 "\n",
			event->urdmad_dev_id, event->kmod_qp_id,
			event->urdmad_qp_id);

	dev = &driver->ports[event->urdmad_dev_id];
	qp = &dev->qp[event->urdmad_qp_id];

	rte_spinlock_lock(&qp->conn_event_lock);
	assert(event->src_port != 0);
	assert(event->src_ipv4 == dev->ipv4_addr);
	assert(event->rxq == qp->rx_queue);
	assert(event->txq == qp->tx_queue);
	qp->local_udp_port = event->src_port;
	qp->local_ipv4_addr = event->src_ipv4;
	qp->remote_udp_port = event->dst_port;
	qp->remote_ipv4_addr = event->dst_ipv4;
	qp->ord_max = event->ord_max;
	qp->ird_max = event->ird_max;
	switch (dev->mtu) {
	case 9000:
		qp->mtu = 8192;
		break;
	default:
		qp->mtu = 1024;
	}
	ret = rte_eth_rx_queue_info_get(event->urdmad_dev_id,
			event->urdmad_qp_id, &rxq_info);
	if (ret < 0) {
		qp->rx_desc_count = dev->rx_desc_count;
	} else {
		qp->rx_desc_count = rxq_info.nb_desc;
	}
	memcpy(&qp->remote_ether_addr, event->dst_ether, ETHER_ADDR_LEN);
	if (dev->flags & port_fdir) {
		memset(&fdirf, 0, sizeof(fdirf));
		fdirf.input.flow_type = RTE_ETH_FLOW_NONFRAG_IPV4_UDP;
		fdirf.input.flow.udp4_flow.ip.dst_ip = dev->ipv4_addr;
		fdirf.action.behavior = RTE_ETH_FDIR_ACCEPT;
		fdirf.action.report_status = RTE_ETH_FDIR_NO_REPORT_STATUS;
		fdirf.soft_id = event->rxq;
		fdirf.action.rx_queue = event->rxq;
		fdirf.input.flow.udp4_flow.dst_port = event->src_port;
		RTE_LOG(DEBUG, USER1, "fdir: assign rx queue %d: IP address %" PRIx32 ", UDP port %" PRIu16 "\n",
					fdirf.action.rx_queue,
					rte_be_to_cpu_32(dev->ipv4_addr),
					rte_be_to_cpu_16(event->src_port));
		ret = rte_eth_dev_filter_ctrl(dev->portid, RTE_ETH_FILTER_FDIR,
				RTE_ETH_FILTER_ADD, &fdirf);
		if (ret != 0) {
			RTE_LOG(CRIT, USER1, "Could not add fdir UDP filter: %s\n",
					rte_strerror(-ret));
			rte_spinlock_unlock(&qp->conn_event_lock);
			return;
		}

		/* Start the queues now that we have bound to an interface */
		ret = rte_eth_dev_rx_queue_start(event->urdmad_dev_id, event->rxq);
		if (ret < 0) {
			RTE_LOG(DEBUG, USER1, "Enable RX queue %u failed: %s\n",
					event->rxq, rte_strerror(ret));
			rte_spinlock_unlock(&qp->conn_event_lock);
			return;
		}

		ret = rte_eth_dev_tx_queue_start(event->urdmad_dev_id, event->txq);
		if (ret < 0) {
			RTE_LOG(DEBUG, USER1, "Enable RX queue %u failed: %s\n",
					event->txq, rte_strerror(ret));
			rte_spinlock_unlock(&qp->conn_event_lock);
			return;
		}
#if 0
	} else {
		char name[RTE_RING_NAMESIZE];
		snprintf(name, RTE_RING_NAMESIZE, "qp%u_rxring",
				qp->qp_id);
		qp->remote_ep.rx_queue = rte_ring_create(name,
				qp->dev->rx_desc_count, rte_socket_id(),
				RING_F_SP_ENQ|RING_F_SC_DEQ);
		if (!qp->rx_queue) {
			RTE_LOG(DEBUG, USER1, "Set up rx ring failed: %s\n",
						rte_strerror(ret));
			atomic_store(&qp->shm_qp->conn_state, usiw_qp_error);
			rte_spinlock_unlock(&qp->shm_qp->conn_event_lock);
			return;
		}
#endif
	}

	atomic_store(&qp->conn_state, usiw_qp_connected);
	rte_spinlock_unlock(&qp->conn_event_lock);

	rtr_event.event_type = SIW_EVENT_QP_RTR;
	rtr_event.kmod_qp_id = event->kmod_qp_id;
	ret = write(driver->chardev.fd, &rtr_event, sizeof(rtr_event));
	if (ret < 0 || (size_t)ret < sizeof(rtr_event)) {
		static bool warned = false;
		if (!warned) {
			if (ret < 0) {
				RTE_LOG(ERR, USER1, "Error writing event file: %s\n",
						strerror(errno));
			} else {
				RTE_LOG(ERR, USER1, "Wrote only %zd/%zu bytes\n",
						ret, sizeof(rtr_event));
			}
			warned = true;
		}
		return;
	}
	RTE_LOG(DEBUG, USER1, "Post RTR event for queue pair %" PRIu32 "; tx_queue=%" PRIu16 " rx_queue=%" PRIu16 "\n",
			event->kmod_qp_id, event->txq, event->rxq);
}	/* handle_qp_connected_event */


static void
chardev_data_ready(struct urdma_fd *fd)
{
	struct urdma_qp_connected_event event;
	struct pollfd pollfd;
	ssize_t ret;

	ret = read(fd->fd, &event, sizeof(event));
	if (ret < 0 && errno == EAGAIN) {
		return;
	}
	if (ret < 0) {
		static bool warned = false;
		if (!warned) {
			if (ret < 0) {
				RTE_LOG(ERR, USER1, "Error reading event file: %s\n",
						strerror(errno));
			}
			warned = true;
		}
		return;
	}

	switch (event.event_type) {
	case SIW_EVENT_QP_CONNECTED:
		handle_qp_connected_event(&event, ret);
		break;
	case SIW_EVENT_QP_DISCONNECTED:
		handle_qp_disconnected_event((struct urdma_qp_disconnected_event *)&event, ret);
		break;
	}
} /* chardev_data_ready */


static int
send_create_qp_resp(struct urdma_process *process, struct urdmad_qp *qp)
{
	struct urdmad_sock_qp_msg msg;
	int ret;

	msg.hdr.opcode = rte_cpu_to_be_32(urdma_sock_create_qp_resp);
	msg.hdr.dev_id = rte_cpu_to_be_16(qp->dev_id);
	msg.hdr.qp_id = rte_cpu_to_be_16(qp->qp_id);
	msg.ptr = rte_cpu_to_be_64((uintptr_t)qp);
	ret = send(process->fd.fd, &msg, sizeof(msg), 0);
	if (ret < 0) {
		return ret;
	} else if (ret == sizeof(msg)) {
		return 0;
	} else {
		errno = EMSGSIZE;
		return -1;
	}
} /* send_create_qp_resp */


static int
handle_hello(struct urdma_process *process, struct urdmad_sock_hello_req *req)
{
	struct urdmad_sock_hello_resp resp;
	ssize_t ret;
	int i;

	if (!reserve_cores(rte_cpu_to_be_32(req->req_lcore_count),
				process->core_mask))
		return -1;

	memset(&resp, 0, sizeof(resp));
	resp.hdr.opcode = rte_cpu_to_be_32(urdma_sock_hello_resp);
	for (i = 0; i < RTE_DIM(resp.lcore_mask); i++) {
		resp.lcore_mask[i] = rte_cpu_to_be_32(process->core_mask[i]);
	}
	ret = send(process->fd.fd, &resp, sizeof(resp), 0);
	if (ret < 0) {
		return ret;
	} else if (ret == sizeof(resp)) {
		return 0;
	} else {
		errno = EMSGSIZE;
		return -1;
	}

	return 0;
} /* handle_hello */


static void
process_data_ready(struct urdma_fd *process_fd)
{
	struct urdma_process *process
		= container_of(process_fd, struct urdma_process, fd);
	struct usiw_port *port;
	union urdmad_sock_any_msg msg;
	struct urdmad_qp *qp, **prev;
	uint16_t dev_id, qp_id;
	ssize_t ret;

	ret = recv(process->fd.fd, &msg, sizeof(msg), 0);
	if (ret < sizeof(struct urdmad_sock_msg)) {
		RTE_LOG(DEBUG, USER1, "EOF or error on fd %d\n", process->fd.fd);
		LIST_FOR_EACH(qp, &process->owned_qps, urdmad__entry, prev) {
			RTE_LOG(DEBUG, USER1, "Return QP %" PRIu16 " to pool\n",
					qp->qp_id);
			LIST_REMOVE(qp, urdmad__entry);
			LIST_INSERT_HEAD(&driver->ports[qp->dev_id].avail_qp,
					qp, urdmad__entry);
		}
		return_lcores(process->core_mask);
		goto err;
	}

	switch (rte_be_to_cpu_32(msg.hdr.opcode)) {
	case urdma_sock_create_qp_req:
		dev_id = rte_be_to_cpu_16(msg.hdr.dev_id);
		if (dev_id > driver->port_count) {
			goto err;
		}
		port = &driver->ports[dev_id];
		qp = port->avail_qp.lh_first;
		LIST_REMOVE(qp, urdmad__entry);
		RTE_LOG(DEBUG, USER1, "CREATE QP dev_id=%" PRIu16 " on fd %d => qp_id=%" PRIu16 "\n",
				dev_id, process->fd.fd, qp->qp_id);
		LIST_INSERT_HEAD(&process->owned_qps, qp, urdmad__entry);
		ret = send_create_qp_resp(process, qp);
		if (ret < 0) {
			goto err;
		}
		break;
	case urdma_sock_destroy_qp_req:
		dev_id = rte_be_to_cpu_16(msg.hdr.dev_id);
		qp_id = rte_be_to_cpu_16(msg.hdr.qp_id);
		RTE_LOG(DEBUG, USER1, "DESTROY QP qp_id=%" PRIu16 " dev_id=%" PRIu16 " on fd %d\n",
				qp_id, dev_id, process->fd.fd);
		if (dev_id > driver->port_count
				|| qp_id > driver->ports[dev_id].max_qp) {
			goto err;
		}
		port = &driver->ports[dev_id];
		qp = &port->qp[qp_id];
		LIST_REMOVE(qp, urdmad__entry);
		LIST_INSERT_HEAD(&driver->ports[dev_id].avail_qp, qp,
					urdmad__entry);
		break;
	case urdma_sock_hello_req:
		fprintf(stderr, "HELLO on fd %d\n", process->fd.fd);
		if (handle_hello(process, &msg.hello_req) < 0) {
			goto err;
		}
		break;
	default:
		RTE_LOG(DEBUG, USER1, "Unknown opcode %" PRIu32 " on fd %d\n",
				rte_be_to_cpu_32(msg.hdr.opcode),
				process->fd.fd);
		goto err;
	}

	return;

err:
	LIST_REMOVE(process, entry);
	close(process->fd.fd);
} /* process_data_ready */


static int
epoll_add(int epoll_fd, struct urdma_fd *fd, int events)
{
	struct epoll_event event;

	memset(&event, 0, sizeof(event));
	event.events = events;
	event.data.ptr = fd;
	return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd->fd, &event);
} /* epoll_add */


static void
listen_data_ready(struct urdma_fd *listen_fd)
{
	struct urdma_process *proc;

	proc = malloc(sizeof(*proc));
	if (!proc) {
		return;
	}

	proc->fd.fd = accept4(listen_fd->fd, NULL, NULL,
			SOCK_NONBLOCK|SOCK_CLOEXEC);
	if (proc->fd.fd < 0) {
		return;
	}
	proc->fd.data_ready = &process_data_ready;
	if (epoll_add(driver->epoll_fd, &proc->fd, EPOLLIN) < 0) {
		rte_exit(EXIT_FAILURE, "Could not add socket to epoll set: %s\n",
				strerror(errno));
	}
	LIST_INIT(&proc->owned_qps);
	/* This assumes that core_mask is an array member, not a pointer to an
	 * array */
	memset(proc->core_mask, 0, sizeof(proc->core_mask));
	LIST_INSERT_HEAD(&driver->processes, proc, entry);
} /* listen_data_ready */


static void
do_poll(int timeout)
{
	struct epoll_event event;
	struct urdma_fd *fd;
	int ret;

	if (timeout) {
		ret = epoll_wait(driver->epoll_fd, &event, 1, timeout);
		if (ret > 0) {
			fd = event.data.ptr;
			RTE_LOG(DEBUG, USER1, "epoll: event %x on fd %d\n",
					event.events, fd->fd);
			fd->data_ready(fd);
		} else if (ret < 0) {
			static bool warned = false;
			if (!warned) {
				RTE_LOG(ERR, USER1, "Error polling event file for reading: %s\n",
						strerror(errno));
				warned = true;
			}
			return;
		}
	}
} /* do_poll */


static void
kni_process_burst(struct usiw_port *port,
		struct rte_mbuf **rxmbuf, int count)
{

	/* TODO: Forward these to the appropriate process */
#if 0
	struct usiw_qp *qp;
	int i, j;
	if (port->ctx && !(port->flags & port_fdir)) {
		for (i = j = 0; i < count; i++) {
			while (i + j < count
					&& (qp = find_matching_qp(port->ctx,
							rxmbuf[i + j]))) {
				/* This implies that qp->ep_default != NULL */
				rte_ring_enqueue(qp->ep_default->rx_queue,
						rxmbuf[i + j]);
				j++;
			}
			if (i + j < count) {
				rxmbuf[i] = rxmbuf[i + j];
			}
		}

		count -= j;
	}
#endif
#ifdef DEBUG_PACKET_HEADERS
	int i;
	RTE_LOG(DEBUG, USER1, "port %d: receive %d packets\n",
			port->portid, count);
	for (i = 0; i < count; ++i)
		rte_pktmbuf_dump(stderr, rxmbuf[i], 128);
#endif
	rte_kni_tx_burst(port->kni, rxmbuf, count);
} /* kni_process_burst */


static int
event_loop(void *arg)
{
	struct usiw_driver *driver = arg;
	struct usiw_port *port;
	struct rte_mbuf *rxmbuf[RX_BURST_SIZE];
	unsigned int count;
	int portid, ret;

	while (1) {
		do_poll(50);
		for (portid = 0; portid < driver->port_count; ++portid) {
			port = &driver->ports[portid];
			ret = rte_kni_handle_request(port->kni);
			if (ret) {
				break;
			}

			count = rte_kni_rx_burst(port->kni,
					rxmbuf, RX_BURST_SIZE);
			if (count) {
				rte_eth_tx_burst(port->portid, 0,
					rxmbuf, count);
			}

			count = rte_eth_rx_burst(port->portid, 0,
						rxmbuf, RX_BURST_SIZE);
			if (count) {
				kni_process_burst(port, rxmbuf, count);
			}
		}
	}

	return EXIT_FAILURE;
}


static int
setup_base_filters(struct usiw_port *iface)
{
	struct rte_eth_fdir_filter_info filter_info;
	int retval;

	memset(&filter_info, 0, sizeof(filter_info));
	filter_info.info_type = RTE_ETH_FDIR_FILTER_INPUT_SET_SELECT;
	filter_info.info.input_set_conf.flow_type
				= RTE_ETH_FLOW_NONFRAG_IPV4_UDP;
	filter_info.info.input_set_conf.inset_size = 2;
	filter_info.info.input_set_conf.field[0]
				= RTE_ETH_INPUT_SET_L3_DST_IP4;
	filter_info.info.input_set_conf.field[1]
				= RTE_ETH_INPUT_SET_L4_UDP_DST_PORT;
	filter_info.info.input_set_conf.op = RTE_ETH_INPUT_SET_SELECT;
	retval = rte_eth_dev_filter_ctrl(iface->portid, RTE_ETH_FILTER_FDIR,
			RTE_ETH_FILTER_SET, &filter_info);
	if (retval != 0) {
		RTE_LOG(CRIT, USER1, "Could not set fdir filter info: %s\n",
				strerror(-retval));
	}
	return retval;
} /* setup_base_filters */


static int
usiw_port_init(struct usiw_port *iface, struct usiw_port_config *port_config)
{
	static const uint32_t rx_checksum_offloads
		= DEV_RX_OFFLOAD_UDP_CKSUM|DEV_RX_OFFLOAD_IPV4_CKSUM;
	static const uint32_t tx_checksum_offloads
		= DEV_TX_OFFLOAD_UDP_CKSUM|DEV_TX_OFFLOAD_IPV4_CKSUM;

	char name[RTE_MEMPOOL_NAMESIZE];
	struct rte_eth_txconf txconf;
	struct rte_eth_rxconf rxconf;
	struct rte_eth_conf port_conf;
	int socket_id;
	int retval;
	uint16_t q;

	/* *** FIXME: *** LOTS of resource leaks on failure */

	socket_id = rte_eth_dev_socket_id(iface->portid);

	if (iface->portid >= rte_eth_dev_count())
		return -EINVAL;

	memset(&port_conf, 0, sizeof(port_conf));
	iface->flags = 0;
	port_conf.rxmode.max_rx_pkt_len = ETHER_MAX_LEN;
	if ((iface->dev_info.tx_offload_capa & tx_checksum_offloads)
			== tx_checksum_offloads) {
		iface->flags |= port_checksum_offload;
	}
	if ((iface->dev_info.rx_offload_capa & rx_checksum_offloads)
			== rx_checksum_offloads) {
		port_conf.rxmode.hw_ip_checksum = 1;
	}
	if (rte_eth_dev_filter_supported(iface->portid,
						RTE_ETH_FILTER_FDIR) == 0) {
		iface->flags |= port_fdir;
		port_conf.fdir_conf.mode = RTE_FDIR_MODE_PERFECT;
		port_conf.fdir_conf.pballoc = RTE_FDIR_PBALLOC_64K;
		port_conf.fdir_conf.mask.ipv4_mask.src_ip = IPv4(0, 0, 0, 0);
		port_conf.fdir_conf.mask.ipv4_mask.dst_ip
						= IPv4(255, 255, 255, 255);
		port_conf.fdir_conf.mask.src_port_mask = 0;
		port_conf.fdir_conf.mask.dst_port_mask = UINT16_MAX;
	} else {
		port_conf.fdir_conf.mode = RTE_FDIR_MODE_NONE;
	}
	iface->max_qp = URDMA_MAX_QP;
	fprintf(stderr, "max_rx_queues %d\n", iface->dev_info.max_rx_queues);
	if (iface->max_qp > iface->dev_info.max_rx_queues) {
		iface->max_qp = iface->dev_info.max_rx_queues;
	}
	fprintf(stderr, "max_tx_queues %d\n", iface->dev_info.max_tx_queues);
	if (iface->max_qp > iface->dev_info.max_tx_queues) {
		iface->max_qp = iface->dev_info.max_tx_queues;
	}

	/* TODO: Do performance testing to determine optimal descriptor
	 * counts */
	/* FIXME: Retry mempool allocations with smaller amounts until it
	 * succeeds, and then base max_qp and rx/tx_desc_count based on that */
	iface->rx_desc_count = iface->dev_info.rx_desc_lim.nb_max;
	if (iface->rx_desc_count > RX_DESC_COUNT_MAX) {
		iface->rx_desc_count = RX_DESC_COUNT_MAX;
	}
	fprintf(stderr, "rx_desc_count %" PRIu16 "\n", iface->rx_desc_count);
	iface->tx_desc_count = iface->dev_info.tx_desc_lim.nb_max;
	if (iface->tx_desc_count > TX_DESC_COUNT_MAX) {
		iface->tx_desc_count = TX_DESC_COUNT_MAX;
	}

	LIST_INIT(&iface->avail_qp);

	iface->qp = rte_calloc("urdma_qp", iface->max_qp + 1,
			sizeof(*iface->qp), 0);
	if (!iface->qp) {
		rte_exit(EXIT_FAILURE, "Cannot allocate QP array: %s\n",
				rte_strerror(rte_errno));
	}
	for (q = 1; q <= iface->max_qp; ++q) {
		iface->qp[q].qp_id = q;
		iface->qp[q].tx_queue = q;
		iface->qp[q].rx_queue = q;
		atomic_init(&iface->qp[q].conn_state, 0);
		rte_spinlock_init(&iface->qp[q].conn_event_lock);
		LIST_INSERT_HEAD(&iface->avail_qp, &iface->qp[q],
				urdmad__entry);
	}

	snprintf(name, RTE_MEMPOOL_NAMESIZE,
			"port_%u_rx_mempool", iface->portid);
	iface->rx_mempool = rte_pktmbuf_pool_create(name,
		2 * iface->max_qp * iface->rx_desc_count,
		0, 0, RTE_PKTMBUF_HEADROOM + port_config->mtu, socket_id);
	if (iface->rx_mempool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create rx mempool with %u mbufs: %s\n",
				2 * iface->max_qp * iface->rx_desc_count,
				rte_strerror(rte_errno));

	snprintf(name, RTE_MEMPOOL_NAMESIZE,
			"port_%u_tx_mempool", iface->portid);
	iface->tx_ddp_mempool = rte_pktmbuf_pool_create(name,
		2 * iface->max_qp * iface->tx_desc_count,
		0, PENDING_DATAGRAM_INFO_SIZE,
		RTE_PKTMBUF_HEADROOM + port_config->mtu, socket_id);
	if (iface->tx_ddp_mempool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create tx mempool with %u mbufs: %s\n",
				2 * iface->max_qp * iface->tx_desc_count,
				rte_strerror(rte_errno));

	/* FIXME: make these actually separate */
	iface->tx_hdr_mempool = iface->tx_ddp_mempool;

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(iface->portid, iface->max_qp + 1,
			iface->max_qp + 1, &port_conf);
	if (retval != 0)
		return retval;

	rte_eth_promiscuous_disable(iface->portid);

	/* Set up control RX queue */
	retval = rte_eth_rx_queue_setup(iface->portid, 0, iface->rx_desc_count,
			socket_id, NULL, iface->rx_mempool);
	if (retval < 0)
		return retval;

	/* Data RX queue startup is deferred */
	memcpy(&rxconf, &iface->dev_info.default_rxconf, sizeof(rxconf));
	rxconf.rx_deferred_start = 1;
	for (q = 1; q <= iface->max_qp; q++) {
		retval = rte_eth_rx_queue_setup(iface->portid, q,
				iface->rx_desc_count, socket_id, &rxconf,
				iface->rx_mempool);
		if (retval < 0) {
			return retval;
		}
	}

	/* Set up control TX queue */
	retval = rte_eth_tx_queue_setup(iface->portid, 0, iface->tx_desc_count,
			socket_id, NULL);
	if (retval < 0)
		return retval;

	/* Data TX queue requires checksum offload, and startup is deferred */
	memcpy(&txconf, &iface->dev_info.default_txconf, sizeof(txconf));
	txconf.txq_flags = ETH_TXQ_FLAGS_NOVLANOFFL
			| ETH_TXQ_FLAGS_NOXSUMSCTP
			| ETH_TXQ_FLAGS_NOXSUMTCP;
	if (!(iface->flags & port_checksum_offload)) {
		RTE_LOG(DEBUG, USER1, "Port %u does not support checksum offload; disabling\n",
				iface->portid);
		txconf.txq_flags |= ETH_TXQ_FLAGS_NOXSUMUDP;
	}
	txconf.tx_deferred_start = 1;
	for (q = 1; q <= iface->max_qp; q++) {
		retval = rte_eth_tx_queue_setup(iface->portid, q,
				iface->tx_desc_count, socket_id, &txconf);
		if (retval < 0)
			return retval;
	}

	if (iface->flags & port_fdir) {
		retval = setup_base_filters(iface);
		if (retval < 0) {
			return retval;
		}
	}

	retval = usiw_port_setup_kni(iface);
	if (retval < 0) {
		return retval;
	}

	retval = rte_eth_dev_set_mtu(iface->portid, port_config->mtu);
	if (retval < 0) {
		rte_exit(EXIT_FAILURE, "Could not set port %u MTU to %u: %s\n",
				iface->portid, port_config->mtu,
				strerror(-retval));
	}
	iface->mtu = port_config->mtu;

	return rte_eth_dev_start(iface->portid);
} /* usiw_port_init */


static void
setup_socket(const char *path)
{
	struct sockaddr_un addr;
	int flags, ret;

	if (strlen(path) >= sizeof(addr.sun_path) - 1) {
		rte_exit(EXIT_FAILURE, "Invalid socket path %s: too long\n",
				path);
	}
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path));

	if (unlink(path) < 0 && errno != ENOENT) {
		rte_exit(EXIT_FAILURE, "Could not unlink previous socket %s: %s\n",
				path, strerror(errno));
	}

	driver->listen.fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (driver->listen.fd < 0) {
		rte_exit(EXIT_FAILURE, "Could not open socket %s: %s\n",
				path, strerror(errno));
	}

	flags = fcntl(driver->listen.fd, F_GETFL);
	if (flags == -1 || fcntl(driver->listen.fd, F_SETFL,
						flags | O_NONBLOCK)) {
		rte_exit(EXIT_FAILURE, "Could not make socket non-blocking: %s\n",
				strerror(errno));
	}

	if (bind(driver->listen.fd, (struct sockaddr *)&addr,
				sizeof(addr)) < 0) {
		rte_exit(EXIT_FAILURE, "Could not bind socket %s: %s\n",
				path, strerror(errno));
	}

	LIST_INIT(&driver->processes);
	if (listen(driver->listen.fd, 16) < 0) {
		rte_exit(EXIT_FAILURE, "Could not listen on socket %s: %s\n",
				path, strerror(errno));
	}

	driver->listen.data_ready = &listen_data_ready;
	if (epoll_add(driver->epoll_fd, &driver->listen, EPOLLIN) < 0) {
		rte_exit(EXIT_FAILURE, "Could not add socket to epoll set: %s\n",
				strerror(errno));
	}
} /* setup_socket */


static void
do_init_driver(void)
{
	struct usiw_port_config *port_config;
	struct usiw_config config;
	char *sock_name;
	int portid, port_count;
	int retval;

	retval = urdma__config_file_open(&config);
	if (retval < 0) {
		rte_exit(EXIT_FAILURE, "Could not open config file: %s\n",
				strerror(errno));
	}

	port_count = rte_eth_dev_count();

	retval = urdma__config_file_get_ports(&config, &port_config);
	if (retval <= 0) {
		rte_exit(EXIT_FAILURE, "Could not parse config file: %s\n",
				strerror(errno));
	} else if (port_count < retval) {
		rte_exit(EXIT_FAILURE, "Configuration expects %d devices but found only %d\n",
				retval, port_count);
	}
	port_count = retval;

	sock_name = urdma__config_file_get_sock_name(&config);
	if (!sock_name) {
		rte_exit(EXIT_FAILURE, "sock_name not found in configuration\n");
	}

	urdma__config_file_close(&config);

	driver = calloc(1, sizeof(*driver)
			+ port_count * sizeof(struct usiw_port));
	if (!driver) {
		rte_exit(EXIT_FAILURE, "Could not allocate main driver structure: %s\n",
				strerror(errno));
	}
	driver->port_count = port_count;
	driver->epoll_fd = epoll_create(EPOLL_CLOEXEC);
	if (driver->epoll_fd < 0) {
		rte_exit(EXIT_FAILURE, "Could not open epoll fd: %s\n",
				strerror(errno));
	}
	setup_socket(sock_name);
	free(sock_name);
	driver->chardev.data_ready = &chardev_data_ready;
	driver->chardev.fd = open("/dev/urdma", O_RDWR|O_NONBLOCK);
	if (driver->chardev.fd < 0) {
		rte_exit(EXIT_FAILURE, "Could not open urdma char device: %s\n",
				strerror(errno));
	}
	if (epoll_add(driver->epoll_fd, &driver->chardev, EPOLLIN) < 0) {
		rte_exit(EXIT_FAILURE, "Could not add urdma char device to epoll set: %s\n",
				strerror(errno));
	}
	rte_kni_init(driver->port_count);

	driver->progress_lcore = 1;
	for (portid = 0; portid < driver->port_count; ++portid) {
		driver->ports[portid].portid = portid;
		rte_eth_macaddr_get(portid,
				&driver->ports[portid].ether_addr);
		rte_eth_dev_info_get(portid, &driver->ports[portid].dev_info);

		retval = usiw_port_init(&driver->ports[portid],
					&port_config[portid]);
		if (retval < 0) {
			rte_exit(EXIT_FAILURE, "Could not initialize port %u: %s\n",
					portid, strerror(-retval));
		}
	}
	rte_eal_remote_launch(event_loop, driver, driver->progress_lcore);
	/* FIXME: cannot free driver beyond this point since it is being
	 * accessed by the event_loop */
	retval = usiw_driver_setup_netlink(driver);
	if (retval < 0) {
		rte_exit(EXIT_FAILURE, "Could not setup KNI context: %s\n",
					strerror(-retval));
	}
	for (portid = 0; portid < driver->port_count; portid++) {
		retval = usiw_set_ipv4_addr(driver, &driver->ports[portid],
				&port_config[portid]);
		if (retval < 0) {
			rte_exit(EXIT_FAILURE, "Could not set port %u IPv4 address: %s\n",
					portid, strerror(-retval));
		}
	}
	free(port_config);
} /* do_init_driver */


int
main(int argc, char *argv[])
{
	/* We cannot access /proc/self/pagemap as non-root if we are not
	 * dumpable
	 *
	 * We do require CAP_NET_ADMIN but there should be minimal risk from
	 * making ourselves dumpable, compared to requiring root priviliges to
	 * run */
	if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) < 0) {
		perror("WARNING: set dumpable flag failed; DPDK may not initialize properly");
	}

	/* rte_eal_init does nothing and returns -1 if it was already called
	 * (although this behavior is not documented).  rte_eal_init also
	 * crashes the whole program if it fails for any other reason, so we
	 * can depend on a negative return code meaning that rte_eal_init was
	 * already called.  This means that a program can accept the default
	 * EAL configuration by not calling rte_eal_init() before calling into
	 * a verbs function, allowing us to work with unmodified verbs
	 * applications. */
	rte_eal_init(argc, argv);

	init_core_mask();

	do_init_driver();

	pause();
} /* main */
