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

#include <sys/queue.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct queued_message {
	LIST_ENTRY(queued_message) entries;
	size_t length;
	char *data;
};

pthread_t event_tid;
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
struct lws *wsi;
LIST_HEAD(msg_queue, queued_message) msg_queue = LIST_HEAD_INITIALIZER(
    msg_queue);

static int
websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
    void *user, void *in, size_t len)
{
	bool need_signal = false;

	switch (reason) {
	case LWS_CALLBACK_CLIENT_ESTABLISHED:
		lws_callback_on_writable(wsi);
		break;

	case LWS_CALLBACK_CLIENT_RECEIVE:
		break;

	case LWS_CALLBACK_CLIENT_WRITEABLE: {
		pthread_mutex_lock(&mtx);
		if (!LIST_EMPTY(&msg_queue)) {
			struct queued_message *msg = LIST_FIRST(&msg_queue);
			lws_write(wsi, (unsigned char *)msg->data + LWS_PRE,
			    msg->length, LWS_WRITE_TEXT);
			LIST_REMOVE(msg, entries);
			free(msg->data);
			free(msg);
			need_signal = true;
		}
		pthread_mutex_unlock(&mtx);
		if (need_signal)
			lws_callback_on_writable(wsi);
		break;
	}
	case LWS_CALLBACK_CLIENT_CLOSED:
		break;

	default:
		break;
	}

	return (0);
}

static const struct lws_protocols protocols[] = {
	{ "scorpi-term", websocket_callback, 0, 1024 }, { NULL, NULL, 0, 0 }
};

static void *
thread_func(void *p)
{
	struct lws_context *lws_context;

	lws_context = (struct lws_context *)p;
	while (1) {
		lws_service(lws_context, 100);
	}
	return NULL;
}

int
vm_connect(const char *path)
{
	struct lws_context_creation_info info = { 0 };
	struct lws_context *lws_context;
	struct lws_client_connect_info connect_info = { 0 };
	char address[255];

	info.port = CONTEXT_PORT_NO_LISTEN;
	info.protocols = protocols;
	info.options = LWS_SERVER_OPTION_UNIX_SOCK;
	info.iface = path;
	info.gid = -1;
	info.uid = -1;

	lws_set_log_level(LLL_ERR, NULL);

	lws_context = lws_create_context(&info);
	if (!lws_context) {
		fprintf(stderr, "Failed to create LWS context\n");
		return (-1);
	}

	snprintf(address, sizeof(address), "+%s", path);
	connect_info.context = lws_context;
	connect_info.protocol = protocols[0].name;
	connect_info.address = address;
	connect_info.port = CONTEXT_PORT_NO_LISTEN;

	wsi = lws_client_connect_via_info(&connect_info);
	if (!wsi) {
		fprintf(stderr, "Failed to connect to the vm comm socket\n");
		return (-1);
	}
	pthread_create(&event_tid, NULL, thread_func, lws_context);
	return (0);
}

int
vm_resize_request(int port, int rows, int cols)
{
	char req[1024];
	struct queued_message *msg;

	if (!wsi) {
		fprintf(stderr, "not connected\n");
		return (-1);
	}

	snprintf(req, sizeof(req),
	    "{"
	    "\"type\": \"request\","
	    "\"id\": 1,"
	    "\"action\": \"term_resize\","
	    "\"args\": [\"%d\", \"%d\", \"%d\"]"
	    "}",
	    port, rows, cols);

	msg = calloc(1, sizeof(*msg));
	msg->data = malloc(strlen(req) + LWS_PRE);
	strcpy(msg->data + LWS_PRE, req);
	msg->length = strlen(req);

	pthread_mutex_lock(&mtx);
	LIST_INSERT_HEAD(&msg_queue, msg, entries);
	lws_callback_on_writable(wsi);
	pthread_mutex_unlock(&mtx);

	return (0);
}
