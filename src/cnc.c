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

#include <sys/types.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cnc.h"
#include "config.h"
#include "debug.h"
#include "mjson.h"

#define BUFFER_SIZE	     1024
#define MAX_REQUEST_SIZE     (64 * 1024)
#define MAX_PENDING_MESSAGES 50
#define MAX_STRLEN	     128
#define MAX_ARGS	     8

struct queued_message {
	STAILQ_ENTRY(queued_message) entries;
	size_t length;
	char *data;
};

struct per_session_data {
	struct lws *wsi;
	STAILQ_HEAD(message_queue, queued_message) queue;
	int queue_size;
	char *rx_data;
	size_t rx_len;
	bool rx_discarding;
	LIST_ENTRY(per_session_data) entries;
	pthread_mutex_t queue_lock;
};

static struct lws_context *global_context = NULL;
static pthread_t server_thread;
static pthread_mutex_t sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t commands_lock = PTHREAD_MUTEX_INITIALIZER;

LIST_HEAD(cmd_set, cnc_command) cmd_set = LIST_HEAD_INITIALIZER(cmd_set);
LIST_HEAD(sessions, per_session_data) sessions = LIST_HEAD_INITIALIZER(
    sessions);

static const struct lws_protocols protocols[];

void
cnc_register_command(const char *cmd, CMD_HANDLER handler, void *param)
{
	struct cnc_command *new_cmd = malloc(sizeof(struct cnc_command));
	if (!new_cmd) {
		EPRINTLN("malloc");
		exit(-1);
	}

	new_cmd->cmd = strdup(cmd);
	new_cmd->cmd_handler = handler;
	new_cmd->param = param;

	pthread_mutex_lock(&commands_lock);
	LIST_INSERT_HEAD(&cmd_set, new_cmd, entries);
	pthread_mutex_unlock(&commands_lock);
}

void
cnc_unregister_commands_by_param(void *param)
{
	struct cnc_command *cmd, *tmp;

	pthread_mutex_lock(&commands_lock);
	LIST_FOREACH_SAFE(cmd, &cmd_set, entries, tmp) {
		if (cmd->param != param)
			continue;
		LIST_REMOVE(cmd, entries);
		free((void *)cmd->cmd);
		free(cmd);
	}
	pthread_mutex_unlock(&commands_lock);
}

void *
server_thread_function(void *arg)
{
	struct lws_context *srv_ctx = (struct lws_context *)arg;
	while (1) {
		lws_service(srv_ctx, 1000);
	}
	return (NULL);
}

int
cnc_start_srv()
{
	const char *socket_path = get_config_value("comm_sock");
	struct lws_context_creation_info info = { 0 };
	info.protocols = protocols;
	info.gid = -1;
	info.uid = -1;
	info.options = LWS_SERVER_OPTION_UNIX_SOCK;

	if (!socket_path)
		return (-1);

	info.iface = socket_path;

	lws_set_log_level(LLL_ERR, NULL);

	global_context = lws_create_context(&info);
	if (!global_context) {
		EPRINTLN("cnc_start_srv: Failed to create LWS context");
		return (-1);
	}

	pthread_create(&server_thread, NULL, server_thread_function,
	    global_context);
	PRINTLN("CNC WebSocket server started at %s", socket_path);
	return (0);
}

static void
cnc_check_queue_size(struct per_session_data *pss)
{
	if (pss->queue_size >= MAX_PENDING_MESSAGES) {
		EPRINTLN(
		    "cnc_check_queue_size: queue is full, dropping a message");
		struct queued_message *old_msg = STAILQ_FIRST(&pss->queue);
		STAILQ_REMOVE_HEAD(&pss->queue, entries);
		free(old_msg->data);
		free(old_msg);
		pss->queue_size--;
	}
}

void
cnc_send_response(struct cnc_conn_t *c, int response_id, const char *data)
{
	struct per_session_data *pss = (struct per_session_data *)c;
	char response[BUFFER_SIZE];
	struct queued_message *msg;

	pthread_mutex_lock(&pss->queue_lock);

	cnc_check_queue_size(pss);

	msg = malloc(sizeof(struct queued_message));
	if (!msg) {
		pthread_mutex_unlock(&pss->queue_lock);
		return;
	}

	snprintf(response, BUFFER_SIZE,
	    "{ \"type\": \"response\", \"id\": %d, \"status\": \"success\", \"data\": %s }",
	    response_id, data);
	msg->length = strlen(response);
	msg->data = malloc(msg->length + LWS_PRE + 1);
	strncpy(msg->data + LWS_PRE, response, msg->length);
	STAILQ_INSERT_TAIL(&pss->queue, msg, entries);
	pss->queue_size++;

	lws_callback_on_writable(pss->wsi);
	pthread_mutex_unlock(&pss->queue_lock);
}

void
cnc_send_notification(const char *data)
{
	struct queued_message *msg;
	struct per_session_data *pss;
	const struct lws_protocols *prot;

	pthread_mutex_lock(&sessions_lock);

	LIST_FOREACH(pss, &sessions, entries) {
		pthread_mutex_lock(&pss->queue_lock);

		prot = lws_get_protocol(pss->wsi);
		if (strcmp(prot->name, protocols[0].name)) {
			pthread_mutex_unlock(&pss->queue_lock);
			continue;
		}

		cnc_check_queue_size(pss);

		msg = malloc(sizeof(struct queued_message));
		if (!msg) {
			EPRINTLN(
			    "cnc_send_notification: failed to allocate memory");
			pthread_mutex_unlock(&pss->queue_lock);
			continue;
		}

		char notification[BUFFER_SIZE];
		snprintf(notification, BUFFER_SIZE,
		    "{ \"type\": \"notification\", \"data\": %s }", data);
		msg->length = strlen(notification);
		msg->data = malloc(msg->length + LWS_PRE + 1);
		strncpy(msg->data + LWS_PRE, notification, msg->length);
		STAILQ_INSERT_TAIL(&pss->queue, msg, entries);

		lws_callback_on_writable(pss->wsi);
		pthread_mutex_unlock(&pss->queue_lock);
	}

	pthread_mutex_unlock(&sessions_lock);
}

static void
cnc_execute_command(struct per_session_data *pss, char *json)
{
	char action[MAX_STRLEN] = "";
	char req_type[MAX_STRLEN] = "";
	char args[MAX_ARGS * MAX_STRLEN];
	char *argv[MAX_ARGS];
	int argc = 0, err, req_id = 0;
	struct cnc_command *cmd;
	CMD_HANDLER handler = NULL;
	void *param = NULL;

	struct json_attr_t json_attrs_req[] = {
		{ "type", t_string, .addr.string = req_type,
		    .len = sizeof(req_type) },
		{ "id", t_integer, .addr.integer = &req_id },
		{ "action", t_string, .addr.string = action,
		    .len = sizeof(action) },
		{ "args", t_array, .addr.array.element_type = t_string,
		    .addr.array.arr.strings = { argv, args,
			MAX_STRLEN * MAX_ARGS },
		    .addr.array.maxlen = MAX_ARGS, .addr.array.count = &argc },
		{ NULL },
	};

	err = json_read_object(json, json_attrs_req, NULL);
	if (err) {
		EPRINTLN("Failed to parse JSON request %s", json);
		return;
	}
	if (strcmp(req_type, "request") != 0) {
		EPRINTLN("Ignoring unexpected JSON message type %s", req_type);
		return;
	}

	pthread_mutex_lock(&commands_lock);
	LIST_FOREACH(cmd, &cmd_set, entries) {
		if (!strcmp(action, cmd->cmd)) {
			handler = cmd->cmd_handler;
			param = (void *)cmd->param;
			break;
		}
	}
	pthread_mutex_unlock(&commands_lock);
	if (handler != NULL)
		handler((struct cnc_conn_t *)pss, req_id, argc, argv, param);
}

static void
cnc_reset_rx_message(struct per_session_data *pss)
{
	free(pss->rx_data);
	pss->rx_data = NULL;
	pss->rx_len = 0;
}

static int
cnc_append_rx_message(struct per_session_data *pss, const char *in,
    size_t len)
{
	char *data;

	if (len > MAX_REQUEST_SIZE - 1 ||
	    pss->rx_len > MAX_REQUEST_SIZE - 1 - len) {
		EPRINTLN("Dropping oversized JSON request");
		cnc_reset_rx_message(pss);
		pss->rx_discarding = true;
		return (-1);
	}

	data = realloc(pss->rx_data, pss->rx_len + len + 1);
	if (data == NULL) {
		EPRINTLN("Failed to allocate JSON request buffer");
		cnc_reset_rx_message(pss);
		pss->rx_discarding = true;
		return (-1);
	}

	pss->rx_data = data;
	memcpy(pss->rx_data + pss->rx_len, in, len);
	pss->rx_len += len;
	pss->rx_data[pss->rx_len] = '\0';
	return (0);
}

static void
cnc_receive_command(struct per_session_data *pss, struct lws *wsi,
    const char *in, size_t len)
{
	bool final = lws_is_final_fragment(wsi) &&
	    lws_remaining_packet_payload(wsi) == 0;

	if (pss->rx_discarding) {
		if (final)
			pss->rx_discarding = false;
		return;
	}

	if (cnc_append_rx_message(pss, in, len) < 0)
		return;

	if (final) {
		cnc_execute_command(pss, pss->rx_data);
		cnc_reset_rx_message(pss);
	}
}

static void
cnc_clean_context(struct per_session_data *pss)
{
	struct per_session_data *tmp_pss;

	pthread_mutex_lock(&pss->queue_lock);
	while (!STAILQ_EMPTY(&pss->queue)) {
		struct queued_message *msg = STAILQ_FIRST(&pss->queue);
		STAILQ_REMOVE_HEAD(&pss->queue, entries);
		free(msg->data);
		free(msg);
	}

	pthread_mutex_unlock(&pss->queue_lock);
	pthread_mutex_destroy(&pss->queue_lock);
	cnc_reset_rx_message(pss);

	pthread_mutex_lock(&sessions_lock);
	LIST_FOREACH(tmp_pss, &sessions, entries) {
		if (tmp_pss->wsi == pss->wsi) {
			LIST_REMOVE(tmp_pss, entries);
			break;
		}
	}
	pthread_mutex_unlock(&sessions_lock);
}

static int
cnc_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user,
    void *in, size_t len)
{
	struct per_session_data *pss = (struct per_session_data *)user;
	bool need_signal = false;

	switch (reason) {
	case LWS_CALLBACK_ESTABLISHED:
		pthread_mutex_lock(&sessions_lock);
		LIST_INSERT_HEAD(&sessions, pss, entries);
		pthread_mutex_unlock(&sessions_lock);

		pss->wsi = wsi;
		STAILQ_INIT(&pss->queue);
		pss->queue_size = 0;
		pss->rx_data = NULL;
		pss->rx_len = 0;
		pss->rx_discarding = false;
		pthread_mutex_init(&pss->queue_lock, NULL);
		break;

	case LWS_CALLBACK_RECEIVE:
		cnc_receive_command(pss, wsi, (const char *)in, len);
		break;

	case LWS_CALLBACK_SERVER_WRITEABLE:
		pthread_mutex_lock(&pss->queue_lock);
		if (!STAILQ_EMPTY(&pss->queue)) {
			struct queued_message *msg = STAILQ_FIRST(&pss->queue);
			lws_write(wsi, (unsigned char *)msg->data + LWS_PRE,
			    msg->length, LWS_WRITE_TEXT);
			STAILQ_REMOVE_HEAD(&pss->queue, entries);
			free(msg->data);
			free(msg);
			pss->queue_size--;
			need_signal = true;
		}
		pthread_mutex_unlock(&pss->queue_lock);
		if (need_signal)
			lws_callback_on_writable(pss->wsi);
		break;

	case LWS_CALLBACK_CLOSED:
		cnc_clean_context(pss);
		break;

	default:
		break;
	}
	return (0);
}

static const struct lws_protocols protocols[] = {
	{ "scorpi-cnc", cnc_callback, sizeof(struct per_session_data),
	    BUFFER_SIZE },
	{ "scorpi-term", cnc_callback, sizeof(struct per_session_data),
	    BUFFER_SIZE },
	{ NULL, NULL, 0, 0 }
};
