/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Vincenzo Maffione <vmaffione@FreeBSD.org>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This file implements multiple network backends (tap, netmap, ...),
 * to be used by network frontends such as virtio-net and e1000.
 * The API to access the backend (e.g. send/receive packets, negotiate
 * features) is exported by net_backends.h.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/uio.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <support/if_tap.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include "config.h"
#include "cnc.h"
#include "debug.h"
#include "iov.h"
#include "mevent.h"
#include "net_backends.h"
#include "net_backends_priv.h"
#include "pci_emul.h"
#include <support/ethernet.h>

#define NET_BE_SIZE(be) (sizeof(*be) + (be)->priv_size)

SET_DECLARE(net_be_set, struct net_backend);

#define IPPROTO_UDP_VALUE 17
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_BOOTREPLY 2
#define DHCP_OPTIONS_OFFSET 236
#define DHCP_MAGIC_COOKIE 0x63825363
#define DHCP_OPTION_MESSAGE_TYPE 53
#define DHCP_MESSAGE_ACK 5
#define MAX_DHCP_INSPECT_BYTES 576

static void
netbe_port_forward_reply(cnc_conn_t conn, int req_id, int error)
{
	char data[128];

	if (error == 0)
		snprintf(data, sizeof(data), "{\"ok\":true}");
	else
		snprintf(data, sizeof(data),
		    "{\"ok\":false,\"error\":\"%s\"}", strerror(error));
	cnc_send_response(conn, req_id, data);
}

static void
netbe_port_forward_command(cnc_conn_t conn, int req_id, int argc,
    char *argv[], bool remove, void *param)
{
	struct net_backend *be = param;
	struct in_addr in;
	struct in_addr requested_guest;
	char guest_addr[INET_ADDRSTRLEN];
	char rule[256];
	int error;
	bool have_requested_guest;

	if (argc != 5) {
		netbe_port_forward_reply(conn, req_id, EINVAL);
		return;
	}
	have_requested_guest = argv[3][0] != '\0';
	if (have_requested_guest &&
	    inet_pton(AF_INET, argv[3], &requested_guest) != 1) {
		netbe_port_forward_reply(conn, req_id, EINVAL);
		return;
	}
	error = 0;

	if (be == NULL || (!remove && be->add_hostfwd == NULL) ||
	    (remove && be->remove_hostfwd == NULL)) {
		netbe_port_forward_reply(conn, req_id, ENOTSUP);
		return;
	}
	if (be->guest_ipv4 == 0 ||
	    (have_requested_guest && be->guest_ipv4 != requested_guest.s_addr)) {
		if (error == 0)
			error = EADDRNOTAVAIL;
		netbe_port_forward_reply(conn, req_id, error);
		return;
	}
	in.s_addr = be->guest_ipv4;
	if (inet_ntop(AF_INET, &in, guest_addr, sizeof(guest_addr)) == NULL) {
		netbe_port_forward_reply(conn, req_id, errno);
		return;
	}

	snprintf(rule, sizeof(rule), "%s:%s:%s-%s:%s", argv[0], argv[1],
	    argv[2], guest_addr, argv[4]);
	error = remove ? be->remove_hostfwd(be, rule) :
			 be->add_hostfwd(be, rule);
	netbe_port_forward_reply(conn, req_id, error);
}

static void
netbe_port_forward_add_command(cnc_conn_t conn, int req_id, int argc,
    char *argv[], void *param)
{
	netbe_port_forward_command(conn, req_id, argc, argv, false, param);
}

static void
netbe_port_forward_remove_command(cnc_conn_t conn, int req_id, int argc,
    char *argv[], void *param)
{
	netbe_port_forward_command(conn, req_id, argc, argv, true, param);
}

static void
netbe_guest_ipv4_command(cnc_conn_t conn, int req_id, int argc __unused,
    char *argv[] __unused, void *param)
{
	struct net_backend *be = param;
	struct in_addr in;
	char addr[INET_ADDRSTRLEN];
	char data[128];

	in.s_addr = 0;
	if (be != NULL)
		in.s_addr = be->guest_ipv4;

	if (in.s_addr == 0) {
		cnc_send_response(conn, req_id, "{\"address\":\"\"}");
		return;
	}
	if (inet_ntop(AF_INET, &in, addr, sizeof(addr)) == NULL) {
		netbe_port_forward_reply(conn, req_id, errno);
		return;
	}
	snprintf(data, sizeof(data), "{\"address\":\"%s\"}", addr);
	cnc_send_response(conn, req_id, data);
}

static void
netbe_command_name(char *out, size_t out_len, const char *prefix,
    const char *adapter)
{
	snprintf(out, out_len, "%s:%s", prefix, adapter);
}

static void
netbe_register_port_forward_commands(struct net_backend *be)
{
	char cmd[128];

	netbe_command_name(cmd, sizeof(cmd), "net_port_forward_add", be->id);
	cnc_register_command(cmd, netbe_port_forward_add_command, be);
	netbe_command_name(cmd, sizeof(cmd), "net_port_forward_remove", be->id);
	cnc_register_command(cmd, netbe_port_forward_remove_command, be);
	netbe_command_name(cmd, sizeof(cmd), "net_guest_ipv4", be->id);
	cnc_register_command(cmd, netbe_guest_ipv4_command, be);
}

static size_t
copy_iov_prefix(const struct iovec *iov, int iovcnt, uint8_t *dst,
    size_t maxlen)
{
	size_t copied, take;

	copied = 0;
	for (int i = 0; i < iovcnt && copied < maxlen; i++) {
		take = iov[i].iov_len;
		if (take > maxlen - copied)
			take = maxlen - copied;
		memcpy(dst + copied, iov[i].iov_base, take);
		copied += take;
	}
	return (copied);
}

static int
dhcp_message_type(const uint8_t *options, size_t options_len)
{
	size_t pos;
	uint32_t cookie;

	if (options_len < 4)
		return (-1);
	memcpy(&cookie, options, sizeof(cookie));
	if (ntohl(cookie) != DHCP_MAGIC_COOKIE)
		return (-1);

	pos = 4;
	while (pos < options_len) {
		uint8_t code, len;

		code = options[pos++];
		if (code == 0)
			continue;
		if (code == 255)
			break;
		if (pos >= options_len)
			break;
		len = options[pos++];
		if (pos + len > options_len)
			break;
		if (code == DHCP_OPTION_MESSAGE_TYPE && len == 1)
			return (options[pos]);
		pos += len;
	}
	return (-1);
}

static uint32_t
dhcp_ack_yiaddr(const uint8_t *pkt, size_t len)
{
	const uint8_t *ip, *udp, *dhcp;
	size_t ip_len, udp_len, dhcp_len;
	uint16_t eth_type, frag, src_port, dst_port;
	uint32_t yiaddr;

	if (len < ETHER_HDR_LEN + 20)
		return (0);

	eth_type = ((uint16_t)pkt[12] << 8) | pkt[13];
	if (eth_type != ETHERTYPE_IP)
		return (0);

	ip = pkt + ETHER_HDR_LEN;
	if ((ip[0] >> 4) != 4)
		return (0);
	ip_len = (ip[0] & 0x0f) * 4;
	if (ip_len < 20 || len < ETHER_HDR_LEN + ip_len + 8)
		return (0);
	if (ip[9] != IPPROTO_UDP_VALUE)
		return (0);
	frag = ((uint16_t)ip[6] << 8) | ip[7];
	if ((frag & 0x3fff) != 0)
		return (0);

	udp = ip + ip_len;
	src_port = ((uint16_t)udp[0] << 8) | udp[1];
	dst_port = ((uint16_t)udp[2] << 8) | udp[3];
	if (src_port != DHCP_SERVER_PORT || dst_port != DHCP_CLIENT_PORT)
		return (0);
	udp_len = ((uint16_t)udp[4] << 8) | udp[5];
	if (udp_len < 8 || len < ETHER_HDR_LEN + ip_len + udp_len)
		return (0);

	dhcp = udp + 8;
	dhcp_len = udp_len - 8;
	if (dhcp_len < DHCP_OPTIONS_OFFSET + 4)
		return (0);
	if (dhcp[0] != DHCP_BOOTREPLY)
		return (0);
	if (dhcp_message_type(dhcp + DHCP_OPTIONS_OFFSET,
		dhcp_len - DHCP_OPTIONS_OFFSET) != DHCP_MESSAGE_ACK)
		return (0);

	memcpy(&yiaddr, dhcp + 16, sizeof(yiaddr));
	return (yiaddr);
}

static void
netbe_maybe_learn_guest_ipv4(struct net_backend *be, const struct iovec *iov,
    int iovcnt, ssize_t len)
{
	uint8_t pkt[MAX_DHCP_INSPECT_BYTES];
	uint32_t guest_ipv4;
	char addr[INET_ADDRSTRLEN];
	char notification[128];
	struct in_addr in;
	size_t copied;

	if (be == NULL || be->guest_ipv4 != 0 || len <= 0)
		return;
	copied = copy_iov_prefix(iov, iovcnt, pkt,
	    (size_t)len < sizeof(pkt) ? (size_t)len : sizeof(pkt));
	guest_ipv4 = dhcp_ack_yiaddr(pkt, copied);
	if (guest_ipv4 == 0 || guest_ipv4 == be->guest_ipv4)
		return;

	be->guest_ipv4 = guest_ipv4;
	if (be->guest_ipv4_learned != NULL)
		be->guest_ipv4_learned(be, guest_ipv4);
	in.s_addr = guest_ipv4;
	if (inet_ntop(AF_INET, &in, addr, sizeof(addr)) == NULL)
		return;
	PRINTLN("learned guest IPv4 address %s from DHCP", addr);
	snprintf(notification, sizeof(notification),
	    "{\"event\":\"guest_ipv4\",\"address\":\"%s\"}", addr);
	cnc_send_notification(notification);
}

#if 0
void
tap_cleanup(struct net_backend *be)
{
	struct tap_priv *priv = NET_BE_PRIV(be);

	if (priv->mevp) {
		mevent_delete(priv->mevp);
	}
	if (be->fd != -1) {
		close(be->fd);
		be->fd = -1;
	}
}

static int
tap_init(struct net_backend *be, const char *devname,
    nvlist_t *nvl __unused, net_be_rxeof_t cb, void *param)
{
	struct tap_priv *priv = NET_BE_PRIV(be);
	char tbuf[80];
	int opt = 1, up = IFF_UP;

	if (cb == NULL) {
		EPRINTLN("TAP backend requires non-NULL callback");
		return (-1);
	}

	strcpy(tbuf, "/dev/");
	strlcat(tbuf, devname, sizeof(tbuf));

	be->fd = open(tbuf, O_RDWR);
	if (be->fd == -1) {
		EPRINTLN("open of tap device %s failed", tbuf);
		goto error;
	}

	/*
	 * Set non-blocking and register for read
	 * notifications with the event loop
	 */
	if (ioctl(be->fd, FIONBIO, &opt) < 0) {
		EPRINTLN("tap device O_NONBLOCK failed");
		goto error;
	}

	if (ioctl(be->fd, VMIO_SIOCSIFFLAGS, up)) {
		EPRINTLN("tap device link up failed");
		goto error;
	}

	memset(priv->bbuf, 0, sizeof(priv->bbuf));
	priv->bbuflen = 0;

	priv->mevp = mevent_add_disabled(be->fd, EVF_READ, cb, param);
	if (priv->mevp == NULL) {
		EPRINTLN("Could not register event");
		goto error;
	}

	return (0);

error:
	tap_cleanup(be);
	return (-1);
}

/*
 * Called to send a buffer chain out to the tap device
 */
ssize_t
tap_send(struct net_backend *be, const struct iovec *iov, int iovcnt)
{
	return (writev(be->fd, iov, iovcnt));
}

ssize_t
tap_peek_recvlen(struct net_backend *be)
{
	struct tap_priv *priv = NET_BE_PRIV(be);
	ssize_t ret;

	if (priv->bbuflen > 0) {
		/*
		 * We already have a packet in the bounce buffer.
		 * Just return its length.
		 */
		return priv->bbuflen;
	}

	/*
	 * Read the next packet (if any) into the bounce buffer, so
	 * that we get to know its length and we can return that
	 * to the caller.
	 */
	ret = read(be->fd, priv->bbuf, sizeof(priv->bbuf));
	if (ret < 0 && errno == EWOULDBLOCK) {
		return (0);
	}

	if (ret > 0)
		priv->bbuflen = ret;

	return (ret);
}

ssize_t
tap_recv(struct net_backend *be, const struct iovec *iov, int iovcnt)
{
	struct tap_priv *priv = NET_BE_PRIV(be);
	ssize_t ret;

	if (priv->bbuflen > 0) {
		/*
		 * A packet is available in the bounce buffer, so
		 * we read it from there.
		 */
		ret = buf_to_iov(priv->bbuf, priv->bbuflen,
		    iov, iovcnt, 0);

		/* Mark the bounce buffer as empty. */
		priv->bbuflen = 0;

		return (ret);
	}

	ret = readv(be->fd, iov, iovcnt);
	if (ret < 0 && errno == EWOULDBLOCK) {
		return (0);
	}

	return (ret);
}

void
tap_recv_enable(struct net_backend *be)
{
	struct tap_priv *priv = NET_BE_PRIV(be);

	mevent_enable(priv->mevp);
}

void
tap_recv_disable(struct net_backend *be)
{
	struct tap_priv *priv = NET_BE_PRIV(be);

	mevent_disable(priv->mevp);
}

uint64_t
tap_get_cap(struct net_backend *be __unused)
{

	return (0); /* no capabilities for now */
}

int
tap_set_cap(struct net_backend *be __unused, uint64_t features,
    unsigned vnet_hdr_len)
{

	return ((features || vnet_hdr_len) ? -1 : 0);
}

static struct net_backend tap_backend = {
	.prefix = "tap",
	.priv_size = sizeof(struct tap_priv),
	.init = tap_init,
	.cleanup = tap_cleanup,
	.send = tap_send,
	.peek_recvlen = tap_peek_recvlen,
	.recv = tap_recv,
	.recv_enable = tap_recv_enable,
	.recv_disable = tap_recv_disable,
	.get_cap = tap_get_cap,
	.set_cap = tap_set_cap,
};

/* A clone of the tap backend, with a different prefix. */
static struct net_backend vmnet_backend = {
	.prefix = "vmnet",
	.priv_size = sizeof(struct tap_priv),
	.init = tap_init,
	.cleanup = tap_cleanup,
	.send = tap_send,
	.peek_recvlen = tap_peek_recvlen,
	.recv = tap_recv,
	.recv_enable = tap_recv_enable,
	.recv_disable = tap_recv_disable,
	.get_cap = tap_get_cap,
	.set_cap = tap_set_cap,
};

DATA_SET(net_be_set, tap_backend);
DATA_SET(net_be_set, vmnet_backend);
#endif

int
netbe_legacy_config(nvlist_t *nvl, const char *opts)
{
	char *backend;
	const char *cp;

	if (opts == NULL)
		return (0);

	cp = strchr(opts, ',');
	if (strchr(opts, '=') != NULL && (cp == NULL || strchr(opts, '=') < cp))
		return (pci_parse_legacy_config(nvl, opts));

	if (cp == NULL) {
		set_config_value_node(nvl, "backend", opts);
		return (0);
	}
	backend = strndup(opts, cp - opts);
	set_config_value_node(nvl, "backend", backend);
	free(backend);
	return (pci_parse_legacy_config(nvl, cp + 1));
}

/*
 * Initialize a backend and attach to the frontend.
 * This is called during frontend initialization.
 *  @ret is a pointer to the backend to be initialized
 *  @devname is the backend-name as supplied on the command line,
 * 	e.g. -s 2:0,frontend-name,backend-name[,other-args]
 *  @cb is the receive callback supplied by the frontend,
 *	and it is invoked in the event loop when a receive
 *	event is generated in the hypervisor,
 *  @param is a pointer to the frontend, and normally used as
 *	the argument for the callback.
 */
int
netbe_init(struct net_backend **ret, nvlist_t *nvl, net_be_rxeof_t cb,
    void *param)
{
	struct net_backend **pbe, *nbe, *tbe = NULL;
	const char *value, *type, *id;
	char *devname;
	int err;

	value = get_config_value_node(nvl, "backend");
	if (value == NULL) {
		return (-1);
	}
	devname = strdup(value);

	/*
	 * Use the type given by configuration if exists; otherwise
	 * use the prefix of the backend as the type.
	 */
	type = get_config_value_node(nvl, "type");
	if (type == NULL)
		type = devname;

	/*
	 * Find the network backend that matches the user-provided
	 * device name. net_backend_set is built using a linker set.
	 */
	SET_FOREACH(pbe, net_be_set)
	{
		if (strncmp(type, (*pbe)->prefix, strlen((*pbe)->prefix)) ==
		    0) {
			tbe = *pbe;
			assert(tbe->init != NULL);
			assert(tbe->cleanup != NULL);
			assert(tbe->send != NULL);
			assert(tbe->recv != NULL);
			assert(tbe->get_cap != NULL);
			assert(tbe->set_cap != NULL);
			break;
		}
	}

	*ret = NULL;
	if (tbe == NULL) {
		free(devname);
		return (EINVAL);
	}

	nbe = calloc(1, NET_BE_SIZE(tbe));
	*nbe = *tbe; /* copy the template */
	nbe->fd = -1;
	nbe->sc = param;
	nbe->be_vnet_hdr_len = 0;
	nbe->fe_vnet_hdr_len = 0;
	id = get_config_value_node(nvl, "id");
	if (id == NULL || id[0] == '\0')
		id = devname;
	nbe->id = strdup(id);
	if (nbe->id == NULL) {
		free(devname);
		free(nbe);
		return (ENOMEM);
	}

	/* Initialize the backend. */
	err = nbe->init(nbe, devname, nvl, cb, param);
	if (err) {
		free(nbe->id);
		free(devname);
		free(nbe);
		return (err);
	}

	*ret = nbe;
	if (nbe->add_hostfwd != NULL || nbe->remove_hostfwd != NULL) {
		netbe_register_port_forward_commands(nbe);
	}
	free(devname);

	return (0);
}

void
netbe_cleanup(struct net_backend *be)
{
	if (be != NULL) {
		cnc_unregister_commands_by_param(be);
		be->cleanup(be);
		free(be->id);
		free(be);
	}
}

uint64_t
netbe_get_cap(struct net_backend *be)
{
	assert(be != NULL);
	return (be->get_cap(be));
}

int
netbe_set_cap(struct net_backend *be, uint64_t features, unsigned vnet_hdr_len)
{
	int ret;

	assert(be != NULL);

	/* There are only three valid lengths, i.e., 0, 10 and 12. */
	if (vnet_hdr_len && vnet_hdr_len != VNET_HDR_LEN &&
	    vnet_hdr_len != (VNET_HDR_LEN - sizeof(uint16_t)))
		return (-1);

	be->fe_vnet_hdr_len = vnet_hdr_len;

	ret = be->set_cap(be, features, vnet_hdr_len);
	assert(be->be_vnet_hdr_len == 0 ||
	    be->be_vnet_hdr_len == be->fe_vnet_hdr_len);

	return (ret);
}

ssize_t
netbe_send(struct net_backend *be, const struct iovec *iov, int iovcnt)
{
	if (be)
		return (be->send(be, iov, iovcnt));
	return (-1);
}

int
netbe_poll(struct net_backend *be, int timeout)
{
	if (be)
		return (be->poll(be, timeout));
	return (-1);
}

/*
 * Try to read a packet from the backend, without blocking.
 * If no packets are available, return 0. In case of success, return
 * the length of the packet just read. Return -1 in case of errors.
 */
ssize_t
netbe_recv(struct net_backend *be, const struct iovec *iov, int iovcnt)
{
	ssize_t len;

	if (be) {
		len = be->recv(be, iov, iovcnt);
		netbe_maybe_learn_guest_ipv4(be, iov, iovcnt, len);
		return (len);
	}
	return (-1);
}

/*
 * Read a packet from the backend and discard it.
 * Returns the size of the discarded packet or zero if no packet was available.
 * A negative error code is returned in case of read error.
 */
ssize_t
netbe_rx_discard(struct net_backend *be)
{
	/*
	 * MP note: the dummybuf is only used to discard frames,
	 * so there is no need for it to be per-vtnet or locked.
	 * We only make it large enough for TSO-sized segment.
	 */
	static uint8_t dummybuf[65536 + 64];
	struct iovec iov;

	iov.iov_base = dummybuf;
	iov.iov_len = sizeof(dummybuf);

	return netbe_recv(be, &iov, 1);
}

void
netbe_rx_disable(struct net_backend *be)
{
	if (be)
		be->recv_disable(be);
}

void
netbe_rx_enable(struct net_backend *be)
{
	if (be)
		be->recv_enable(be);
}

size_t
netbe_get_vnet_hdr_len(struct net_backend *be)
{
	if (be)
		return (be->be_vnet_hdr_len);
	return (0);
}
