/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Alex Fishman <alex@fuse-t.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <assert.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include "common.h"
#include <support/ethernet.h>
#include "debug.h"
#include "net_backends.h"
#include "net_backends_priv.h"
#include <net/ethernet.h>   // still provides struct ether_addr

#ifndef ether_addr_t
typedef struct ether_addr ether_addr_t;
#endif
#include <vmnet/vmnet.h>

struct vmnet_priv {
	uint64_t mtu;
	uint64_t max_packet_size;
	uint8_t mac[ETHER_ADDR_LEN];

	interface_ref iface;

	net_be_rxeof_t cb;
	void *cb_param;

	int rx_enabled;
};

void
vmnet_cleanup(struct net_backend *be)
{
}

void
vmnet_drop_packets(struct vmnet_priv *priv)
{
	vmnet_return_t res;
	struct vmpktdesc v;
	int pktcnt;
	struct iovec iov;
	uint8_t tmp_buf[4096];

	while (1) {
		iov.iov_base = tmp_buf;
		iov.iov_len = sizeof(tmp_buf);
		v.vm_pkt_size = sizeof(tmp_buf);

		v.vm_pkt_iov = &iov;
		v.vm_pkt_iovcnt = 1;
		v.vm_flags = 0;

		pktcnt = 1;
		res = vmnet_read(priv->iface, &v, &pktcnt);
		if (pktcnt < 1 || res != VMNET_SUCCESS)
			break;
	}
}

static void
vmnet_recv_cb(struct vmnet_priv *priv)
{
	if (priv->rx_enabled) {
		priv->cb(0, EVF_WRITE, priv->cb_param);
	} else {
		vmnet_drop_packets(priv);
	}
}

static int
vmnet_init(struct net_backend *be, const char *devname, nvlist_t *nvl __unused,
    net_be_rxeof_t cb, void *param)
{
	struct vmnet_priv *priv = NET_BE_PRIV(be);
	dispatch_semaphore_t ds;
	xpc_object_t iface_desc;
	dispatch_queue_t ifq;
	uuid_t uuid;
	__block vmnet_return_t iface_status;

	priv->cb = cb;
	priv->cb_param = param;

	priv->rx_enabled = 0;

	ifq = dispatch_queue_create("scorpi.vmnet.com", DISPATCH_QUEUE_SERIAL);

	iface_desc = xpc_dictionary_create(NULL, NULL, 0);
	xpc_dictionary_set_uint64(iface_desc, vmnet_operation_mode_key,
	    VMNET_SHARED_MODE);

	uuid_generate_random(uuid);
	xpc_dictionary_set_uuid(iface_desc, vmnet_interface_id_key, uuid);
	xpc_dictionary_set_bool(iface_desc, vmnet_enable_checksum_offload_key,
	    true);

	ds = dispatch_semaphore_create(0);

	priv->iface = vmnet_start_interface(iface_desc, ifq,
	    ^(vmnet_return_t status, xpc_object_t interface_param) {
		iface_status = status;
		if (status != VMNET_SUCCESS || !interface_param) {
			dispatch_semaphore_signal(ds);
			return;
		}

		if (sscanf(xpc_dictionary_get_string(interface_param,
			       vmnet_mac_address_key),
			"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &priv->mac[0],
			&priv->mac[1], &priv->mac[2], &priv->mac[3],
			&priv->mac[4], &priv->mac[5]) != 6) {
			assert(0);
		}

		priv->mtu = xpc_dictionary_get_uint64(interface_param,
		    vmnet_mtu_key);
		priv->max_packet_size = xpc_dictionary_get_uint64(
		    interface_param, vmnet_max_packet_size_key);
		dispatch_semaphore_signal(ds);
	    });

	dispatch_semaphore_wait(ds, DISPATCH_TIME_FOREVER);
	dispatch_release(ds);
	if (iface_status != VMNET_SUCCESS)
		goto error;

	vmnet_interface_set_event_callback(priv->iface,
	    VMNET_INTERFACE_PACKETS_AVAILABLE, ifq,
	    ^(interface_event_t event_id, xpc_object_t event) {
		vmnet_recv_cb(priv);
	    });

	return (0);

error:
	vmnet_cleanup(be);
	return (-1);
}

/*
 * Called to send a buffer chain out to the tap device
 */
ssize_t
vmnet_send(struct net_backend *be, const struct iovec *iov, int iovcnt)
{
	vmnet_return_t r;
	struct vmpktdesc v;
	int pktcnt;
	int i;
	struct vmnet_priv *priv = NET_BE_PRIV(be);

	v.vm_pkt_size = 0;

	for (i = 0; i < iovcnt; i++) {
		v.vm_pkt_size += iov[i].iov_len;
	}

	v.vm_pkt_iov = (struct iovec *)iov;
	v.vm_pkt_iovcnt = (uint32_t)iovcnt;
	v.vm_flags = 0;

	pktcnt = 1;

	r = vmnet_write(priv->iface, &v, &pktcnt);
	assert(r == VMNET_SUCCESS);

	return (pktcnt);
}

int
vmnet_poll(struct net_backend *be, int timeout)
{
#if 0
	struct vmnet_priv *priv = NET_BE_PRIV(be);
	int ret;
    struct timeval tv;
    struct timespec ts;
    
    gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec;
    ts.tv_nsec = timeout * 1000000; // ms

    if (timeout != -1)
        ret = pthread_cond_timedwait(&priv->poll_cond, &priv->m, &ts);
    else
        ret = pthread_cond_wait(&priv->poll_cond, &priv->m);
	return (ret);
#endif
	return (0);
}

ssize_t
vmnet_recv(struct net_backend *be, const struct iovec *iov, int iovcnt)
{
	struct vmnet_priv *priv = NET_BE_PRIV(be);
	vmnet_return_t res;
	struct vmpktdesc v;
	int pktcnt;

	v.vm_pkt_size = 0;

	for (int i = 0; i < iovcnt; i++) {
		v.vm_pkt_size += iov[i].iov_len;
	}

	v.vm_pkt_iov = (struct iovec *)iov;
	v.vm_pkt_iovcnt = (uint32_t)iovcnt;
	v.vm_flags = 0;

	pktcnt = 1;
	res = vmnet_read(priv->iface, &v, &pktcnt);
	assert(res == VMNET_SUCCESS);

	if (pktcnt < 1) {
		return (-1);
	}

	return ((ssize_t)v.vm_pkt_size);
}

void
vmnet_recv_enable(struct net_backend *be)
{
	struct vmnet_priv *priv = NET_BE_PRIV(be);
	priv->rx_enabled = 1;
}

void
vmnet_recv_disable(struct net_backend *be)
{
	struct vmnet_priv *priv = NET_BE_PRIV(be);
	priv->rx_enabled = 0;
}

uint64_t
vmnet_get_cap(struct net_backend *be __unused)
{
	return (0); /* no capabilities for now */
}

int
vmnet_set_cap(struct net_backend *be __unused, uint64_t features,
    unsigned vnet_hdr_len)
{
	return ((features || vnet_hdr_len) ? -1 : 0);
}

static struct net_backend vmnet_backend = {
	.prefix = "vmnet",
	.priv_size = sizeof(struct vmnet_priv),
	.init = vmnet_init,
	.cleanup = vmnet_cleanup,
	.send = vmnet_send,
	.poll = vmnet_poll,
	.recv = vmnet_recv,
	.recv_enable = vmnet_recv_enable,
	.recv_disable = vmnet_recv_disable,
	.get_cap = vmnet_get_cap,
	.set_cap = vmnet_set_cap,
};

DATA_SET(net_be_set, vmnet_backend);