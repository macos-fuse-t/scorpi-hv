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
#include "mevent.h"
#include "config.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

struct sock_priv {
	net_be_rxeof_t cb;
	void *cb_param;
	int rx_enabled;
	int sockfd;
	struct mevent *mevp;
};

void
sockbe_cleanup(struct net_backend *be)
{
	struct sock_priv *priv = NET_BE_PRIV(be);

	close(priv->sockfd);
	priv->sockfd = 0;
}

#define SOCKET_PATH "/var/run/scorpi_netd.sock"

int test() {
    // Create and connect socket
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }

    // Create socket pair to send
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        close(sock);
        return -1;
    }

    // Prepare JSON payload
    const char *json = "{\"action\":\"send-fd\",\"params\":{\"client\":\"c-client\"}}";
    
    // Format HTTP request with JSON body
    char http_request[512];
    int http_len = snprintf(http_request, sizeof(http_request),
        "POST /create HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n\r\n"
        "%s", strlen(json), json);

    // Send HTTP headers and body in one write
    if (write(sock, http_request, http_len) != http_len) {
        perror("write");
        close(sock);
        close(sv[0]);
        close(sv[1]);
        return -1;
    }

	usleep(100000); // 100 ms

    // Prepare FD passing message
    struct msghdr msg = {0};
    struct iovec iov;
    char dummy = '!'; // Need at least 1 byte of data
    
    // Setup message structure
    iov.iov_base = &dummy;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);
    
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &sv[1], sizeof(int));

    // Send FD in separate message
    if (sendmsg(sock, &msg, 0) < 0) {
        perror("sendmsg");
        close(sock);
        close(sv[0]);
        close(sv[1]);
        return -1;
    }

    printf("Sent FD %d\n", sv[1]);
    close(sv[1]); // FD has been transferred

    // Read response
    char response[1024];
    ssize_t n = read(sock, response, sizeof(response)-1);
    if (n > 0) {
        response[n] = 0;
        printf("Server response:\n%s\n", response);
    }

    close(sock);

	fcntl(sv[0], F_SETFL, O_NONBLOCK);
    return sv[0]; // Return the remaining socket
}

static int
sockbe_init(struct net_backend *be, const char *devname, nvlist_t *nvl,
    net_be_rxeof_t cb, void *param)
{
	struct sock_priv *priv = NET_BE_PRIV(be);
	const char *fdval;

	priv->cb = cb;
	priv->cb_param = param;
	priv->rx_enabled = 0;

	printf("sockbe_init\n");
	fdval = get_config_value_node(nvl, "fd");
	if (fdval != NULL)
		priv->sockfd = atoi(fdval);
	if (priv->sockfd <= 0) {
		EPRINTLN("Invalid socket fd");
		return (-1);
	}

	priv->sockfd = test();
	if (priv->sockfd <= 0) {
		EPRINTLN("2. Invalid socket fd");
		return (-1);
	}
	
	priv->mevp = mevent_add_disabled(priv->sockfd, EVF_READ, cb, param);
	if (priv->mevp == NULL) {
		EPRINTLN("Could not register event");
		goto error;
	}

	return (0);

error:
	sockbe_cleanup(be);
	return (-1);
}

/*
 * Called to send a buffer chain out to the tap device
 */
ssize_t
sockbe_send(struct net_backend *be, const struct iovec *iov, int iovcnt)
{
	struct sock_priv *priv = NET_BE_PRIV(be);

	return (writev(priv->sockfd, iov, iovcnt));
}

int
sockbe_poll(struct net_backend *be, int timeout)
{
	return (0);
}

ssize_t
sockbe_recv(struct net_backend *be, const struct iovec *iov, int iovcnt)
{
	struct sock_priv *priv = NET_BE_PRIV(be);

	return (readv(priv->sockfd, iov, iovcnt));
}

void
sockbe_recv_enable(struct net_backend *be)
{
	struct sock_priv *priv = NET_BE_PRIV(be);
	priv->rx_enabled = 1;
	mevent_enable(priv->mevp);
}

void
sockbe_recv_disable(struct net_backend *be)
{
	struct sock_priv *priv = NET_BE_PRIV(be);
	priv->rx_enabled = 0;
	mevent_disable(priv->mevp);
}

uint64_t
sockbe_get_cap(struct net_backend *be __unused)
{
	return (0); /* no capabilities for now */
}

int
sockbe_set_cap(struct net_backend *be __unused, uint64_t features,
    unsigned vnet_hdr_len)
{
	return ((features || vnet_hdr_len) ? -1 : 0);
}

static struct net_backend sock_backend = {
	.prefix = "sock",
	.priv_size = sizeof(struct sock_priv),
	.init = sockbe_init,
	.cleanup = sockbe_cleanup,
	.send = sockbe_send,
	.poll = sockbe_poll,
	.recv = sockbe_recv,
	.recv_enable = sockbe_recv_enable,
	.recv_disable = sockbe_recv_disable,
	.get_cap = sockbe_get_cap,
	.set_cap = sockbe_set_cap,
};

DATA_SET(net_be_set, sock_backend);
