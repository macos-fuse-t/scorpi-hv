/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/queue.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cnc.h"
#include "debug.h"
#include "virtio_external_backend.h"

#define VIRTIO_EXT_NAME_MAX 64

struct virtio_external_backend {
	char backend_id[VIRTIO_EXT_NAME_MAX];
	char device_name[VIRTIO_EXT_NAME_MAX];
	char protocol[VIRTIO_EXT_NAME_MAX];
	cnc_conn_t conn;
	LIST_ENTRY(virtio_external_backend) entries;
};

static LIST_HEAD(virtio_external_backends, virtio_external_backend) backends =
    LIST_HEAD_INITIALIZER(backends);
static pthread_mutex_t backends_lock = PTHREAD_MUTEX_INITIALIZER;

static struct virtio_external_backend *
virtio_external_backend_find_locked(const char *backend_id)
{
	struct virtio_external_backend *backend;

	LIST_FOREACH(backend, &backends, entries) {
		if (strcmp(backend->backend_id, backend_id) == 0)
			return (backend);
	}
	return (NULL);
}

bool
virtio_external_backend_registered(const char *backend_id)
{
	bool registered;

	pthread_mutex_lock(&backends_lock);
	registered = virtio_external_backend_find_locked(backend_id) != NULL;
	pthread_mutex_unlock(&backends_lock);
	return (registered);
}

static void
virtio_external_backend_send_not_ready(cnc_conn_t c, int req_id)
{
	cnc_send_response(c, req_id,
	    "{\"accepted\":false,\"reason\":\"not_ready\"}");
}

static void
virtio_backend_register(cnc_conn_t c, int req_id, int argc, char *argv[],
    void *param)
{
	struct virtio_external_backend *backend;

	(void)param;
	if (argc < 3) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_args\"}");
		return;
	}

	pthread_mutex_lock(&backends_lock);
	backend = virtio_external_backend_find_locked(argv[0]);
	if (backend != NULL) {
		backend->conn = c;
		snprintf(backend->device_name, sizeof(backend->device_name), "%s",
		    argv[1]);
		snprintf(backend->protocol, sizeof(backend->protocol), "%s",
		    argv[2]);
		pthread_mutex_unlock(&backends_lock);
		cnc_send_response(c, req_id,
		    "{\"accepted\":true,\"updated\":true}");
		return;
	}

	backend = calloc(1, sizeof(*backend));
	if (backend == NULL) {
		pthread_mutex_unlock(&backends_lock);
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"oom\"}");
		return;
	}

	snprintf(backend->backend_id, sizeof(backend->backend_id), "%s",
	    argv[0]);
	snprintf(backend->device_name, sizeof(backend->device_name), "%s",
	    argv[1]);
	snprintf(backend->protocol, sizeof(backend->protocol), "%s", argv[2]);
	backend->conn = c;
	LIST_INSERT_HEAD(&backends, backend, entries);
	pthread_mutex_unlock(&backends_lock);

	PRINTLN("virtio external backend registered: id=%s device=%s protocol=%s",
	    backend->backend_id, backend->device_name, backend->protocol);
	cnc_send_response(c, req_id, "{\"accepted\":true,\"updated\":false}");
}

static void
virtio_backend_disconnect(cnc_conn_t c, int req_id, int argc, char *argv[],
    void *param)
{
	struct virtio_external_backend *backend;

	(void)param;
	if (argc < 1) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_args\"}");
		return;
	}

	pthread_mutex_lock(&backends_lock);
	backend = virtio_external_backend_find_locked(argv[0]);
	if (backend == NULL) {
		pthread_mutex_unlock(&backends_lock);
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"not_registered\"}");
		return;
	}

	LIST_REMOVE(backend, entries);
	pthread_mutex_unlock(&backends_lock);

	PRINTLN("virtio external backend disconnected: id=%s",
	    backend->backend_id);
	free(backend);
	cnc_send_response(c, req_id, "{\"accepted\":true}");
}

static void
virtio_device_describe(cnc_conn_t c, int req_id, int argc, char *argv[],
    void *param)
{
	(void)argc;
	(void)argv;
	(void)param;
	virtio_external_backend_send_not_ready(c, req_id);
}

static void
virtio_queue_kick(cnc_conn_t c, int req_id, int argc, char *argv[],
    void *param)
{
	(void)argc;
	(void)argv;
	(void)param;
	virtio_external_backend_send_not_ready(c, req_id);
}

static void
virtio_queue_interrupt(cnc_conn_t c, int req_id, int argc, char *argv[],
    void *param)
{
	(void)argc;
	(void)argv;
	(void)param;
	virtio_external_backend_send_not_ready(c, req_id);
}

static void
virtio_device_reset(cnc_conn_t c, int req_id, int argc, char *argv[],
    void *param)
{
	(void)argc;
	(void)argv;
	(void)param;
	virtio_external_backend_send_not_ready(c, req_id);
}

void
virtio_external_backend_init(void)
{
	static int once;

	if (once)
		return;

	cnc_register_command("virtio_backend_register",
	    virtio_backend_register, NULL);
	cnc_register_command("virtio_device_describe",
	    virtio_device_describe, NULL);
	cnc_register_command("virtio_queue_kick", virtio_queue_kick, NULL);
	cnc_register_command("virtio_queue_interrupt",
	    virtio_queue_interrupt, NULL);
	cnc_register_command("virtio_device_reset", virtio_device_reset, NULL);
	cnc_register_command("virtio_backend_disconnect",
	    virtio_backend_disconnect, NULL);
	once = 1;
}
