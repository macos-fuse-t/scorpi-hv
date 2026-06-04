/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/queue.h>
#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cnc.h"
#include "debug.h"
#include "virtio_external_backend.h"

#define VIRTIO_EXT_RESPONSE_MAX 1024

struct virtio_external_backend {
	char backend_id[SCORPI_VIRTIO_EXTERNAL_NAME_MAX];
	char device_name[SCORPI_VIRTIO_EXTERNAL_NAME_MAX];
	char protocol[SCORPI_VIRTIO_EXTERNAL_NAME_MAX];
	cnc_conn_t conn;
	bool connected;
	struct scorpi_virtio_external_transport_desc transport;
	virtio_external_interrupt_cb interrupt_cb;
	virtio_external_reset_cb reset_cb;
	void *device_opaque;
	LIST_ENTRY(virtio_external_backend) entries;
};

static LIST_HEAD(virtio_external_backends,
    virtio_external_backend) backends = LIST_HEAD_INITIALIZER(backends);
static pthread_mutex_t backends_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t virtio_external_append_queue_json(char *buf, size_t len,
    size_t used, const struct scorpi_virtio_external_queue_desc *queue);
static size_t virtio_external_append_memory_json(char *buf, size_t len,
    size_t used,
    const struct scorpi_virtio_external_memory_region_desc *region);

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

static bool
virtio_external_valid_name(const char *name)
{
	if (name == NULL || name[0] == '\0')
		return (false);

	for (const unsigned char *p = (const unsigned char *)name; *p != '\0';
	    p++) {
		if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.')
			continue;
		return (false);
	}
	return (true);
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

int
virtio_external_backend_set_transport(const char *backend_id,
    const struct scorpi_virtio_external_transport_desc *transport)
{
	struct virtio_external_backend *backend;
	int rc = 0;

	if (backend_id == NULL || transport == NULL)
		return (-1);
	if (transport->queue_count > SCORPI_VIRTIO_EXTERNAL_MAX_QUEUES ||
	    transport->memory_region_count >
		SCORPI_VIRTIO_EXTERNAL_MAX_MEMORY_REGIONS)
		return (-1);

	pthread_mutex_lock(&backends_lock);
	backend = virtio_external_backend_find_locked(backend_id);
	if (backend == NULL) {
		backend = calloc(1, sizeof(*backend));
		if (backend == NULL) {
			rc = -1;
			goto done;
		}
		snprintf(backend->backend_id, sizeof(backend->backend_id), "%s",
		    backend_id);
		LIST_INSERT_HEAD(&backends, backend, entries);
	}
	backend->transport = *transport;
	snprintf(backend->transport.backend_id,
	    sizeof(backend->transport.backend_id), "%s", backend_id);
	if (transport->device_name[0] != '\0')
		snprintf(backend->device_name, sizeof(backend->device_name),
		    "%s", transport->device_name);
	if (backend->device_name[0] != '\0')
		snprintf(backend->transport.device_name,
		    sizeof(backend->transport.device_name), "%s",
		    backend->device_name);
done:
	pthread_mutex_unlock(&backends_lock);
	return (rc);
}

int
virtio_external_backend_bind_device(const char *backend_id,
    const struct scorpi_virtio_external_transport_desc *transport,
    virtio_external_interrupt_cb interrupt_cb,
    virtio_external_reset_cb reset_cb, void *opaque)
{
	struct virtio_external_backend *backend;
	int rc;

	rc = virtio_external_backend_set_transport(backend_id, transport);
	if (rc != 0)
		return (rc);

	pthread_mutex_lock(&backends_lock);
	backend = virtio_external_backend_find_locked(backend_id);
	if (backend == NULL) {
		pthread_mutex_unlock(&backends_lock);
		return (-1);
	}
	backend->interrupt_cb = interrupt_cb;
	backend->reset_cb = reset_cb;
	backend->device_opaque = opaque;
	pthread_mutex_unlock(&backends_lock);
	return (0);
}

void
virtio_external_backend_clear_transport(const char *backend_id)
{
	struct virtio_external_backend *backend;

	pthread_mutex_lock(&backends_lock);
	backend = virtio_external_backend_find_locked(backend_id);
	if (backend != NULL)
		memset(&backend->transport, 0, sizeof(backend->transport));
	pthread_mutex_unlock(&backends_lock);
}

int
virtio_external_backend_notify_queue_kick(const char *backend_id,
    uint32_t queue_index)
{
	struct virtio_external_backend *backend;
	char notification[VIRTIO_EXT_RESPONSE_MAX];
	size_t used;
	int rc;

	if (backend_id == NULL)
		return (-1);

	pthread_mutex_lock(&backends_lock);
	backend = virtio_external_backend_find_locked(backend_id);
	if (backend == NULL) {
		pthread_mutex_unlock(&backends_lock);
		return (-1);
	}

	rc = snprintf(notification, sizeof(notification),
	    "{ \"event\": \"virtio_queue_kick\", \"data\": {"
	    "\"backend_id\": \"%s\","
	    "\"device_name\": \"%s\","
	    "\"queue_index\": %u,"
	    "\"reset_generation\": %u,"
	    "\"queues\":[",
	    backend->backend_id, backend->device_name, queue_index,
	    backend->transport.reset_generation);
	if (rc < 0 || (size_t)rc >= sizeof(notification))
		goto too_large;
	used = (size_t)rc;

	for (uint32_t i = 0; i < backend->transport.queue_count; i++)
		used = virtio_external_append_queue_json(notification,
		    sizeof(notification), used, &backend->transport.queues[i]);
	if (used >= sizeof(notification))
		goto too_large;

	rc = snprintf(notification + used, sizeof(notification) - used,
	    "],\"memory_regions\":[");
	if (rc < 0 || (size_t)rc >= sizeof(notification) - used)
		goto too_large;
	used += (size_t)rc;

	for (uint32_t i = 0; i < backend->transport.memory_region_count; i++)
		used = virtio_external_append_memory_json(notification,
		    sizeof(notification), used,
		    &backend->transport.memory_regions[i]);
	if (used >= sizeof(notification))
		goto too_large;

	rc = snprintf(notification + used, sizeof(notification) - used, "]} }");
	if (rc < 0 || (size_t)rc >= sizeof(notification) - used)
		goto too_large;
	pthread_mutex_unlock(&backends_lock);

	cnc_send_notification(notification);
	return (0);

too_large:
	pthread_mutex_unlock(&backends_lock);
	return (-1);
}

int
virtio_external_backend_notify_display_resize(const char *backend_id,
    uint32_t width, uint32_t height)
{
	struct virtio_external_backend *backend;
	char notification[VIRTIO_EXT_RESPONSE_MAX];
	int rc;

	if (backend_id == NULL)
		return (-1);

	pthread_mutex_lock(&backends_lock);
	backend = virtio_external_backend_find_locked(backend_id);
	if (backend == NULL) {
		pthread_mutex_unlock(&backends_lock);
		return (-1);
	}

	rc = snprintf(notification, sizeof(notification),
	    "{ \"event\": \"virtio_gpu_resize\", \"data\": {"
	    "\"backend_id\": \"%s\","
	    "\"device_name\": \"%s\","
	    "\"width\": %u,"
	    "\"height\": %u,"
	    "\"reset_generation\": %u"
	    "} }",
	    backend->backend_id, backend->device_name, width, height,
	    backend->transport.reset_generation);
	if (rc < 0 || (size_t)rc >= sizeof(notification)) {
		pthread_mutex_unlock(&backends_lock);
		return (-1);
	}
	pthread_mutex_unlock(&backends_lock);

	cnc_send_notification(notification);
	return (0);
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
	if (!virtio_external_valid_name(argv[0]) ||
	    !virtio_external_valid_name(argv[1]) ||
	    !virtio_external_valid_name(argv[2])) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_name\"}");
		return;
	}

	pthread_mutex_lock(&backends_lock);
	backend = virtio_external_backend_find_locked(argv[0]);
	if (backend != NULL) {
		backend->conn = c;
		backend->connected = true;
		snprintf(backend->device_name, sizeof(backend->device_name),
		    "%s", argv[1]);
		snprintf(backend->protocol, sizeof(backend->protocol), "%s",
		    argv[2]);
		snprintf(backend->transport.backend_id,
		    sizeof(backend->transport.backend_id), "%s", argv[0]);
		snprintf(backend->transport.device_name,
		    sizeof(backend->transport.device_name), "%s", argv[1]);
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
	backend->connected = true;
	snprintf(backend->transport.backend_id,
	    sizeof(backend->transport.backend_id), "%s", argv[0]);
	snprintf(backend->transport.device_name,
	    sizeof(backend->transport.device_name), "%s", argv[1]);
	LIST_INSERT_HEAD(&backends, backend, entries);
	pthread_mutex_unlock(&backends_lock);

	PRINTLN(
	    "virtio external backend registered: id=%s device=%s protocol=%s",
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
	if (!virtio_external_valid_name(argv[0])) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_name\"}");
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
	PRINTLN("virtio external backend disconnected: id=%s",
	    backend->backend_id);
	if (backend->transport.ready || backend->transport.queue_count > 0 ||
	    backend->transport.memory_region_count > 0) {
		backend->conn = NULL;
		backend->connected = false;
		backend->protocol[0] = '\0';
		LIST_INSERT_HEAD(&backends, backend, entries);
	} else {
		free(backend);
	}
	pthread_mutex_unlock(&backends_lock);
	cnc_send_response(c, req_id, "{\"accepted\":true}");
}

static size_t
virtio_external_append_queue_json(char *buf, size_t len, size_t used,
    const struct scorpi_virtio_external_queue_desc *queue)
{
	int rc;

	rc = snprintf(buf + used, len - used,
	    "%s{\"index\":%u,\"size\":%u,\"desc_addr\":%llu,"
	    "\"avail_addr\":%llu,\"used_addr\":%llu,\"ready\":%s}",
	    used > 0 && buf[used - 1] != '[' ? "," : "", queue->index,
	    queue->size, (unsigned long long)queue->desc_addr,
	    (unsigned long long)queue->avail_addr,
	    (unsigned long long)queue->used_addr,
	    queue->ready ? "true" : "false");
	if (rc < 0 || (size_t)rc >= len - used)
		return (len);
	return (used + (size_t)rc);
}

static size_t
virtio_external_append_memory_json(char *buf, size_t len, size_t used,
    const struct scorpi_virtio_external_memory_region_desc *region)
{
	int rc;

	rc = snprintf(buf + used, len - used,
	    "%s{\"index\":%u,\"guest_phys_addr\":%llu,\"size\":%llu,"
	    "\"shm_name\":\"%s\",\"shm_offset\":%llu}",
	    used > 0 && buf[used - 1] != '[' ? "," : "", region->index,
	    (unsigned long long)region->guest_phys_addr,
	    (unsigned long long)region->size, region->shm_name,
	    (unsigned long long)region->shm_offset);
	if (rc < 0 || (size_t)rc >= len - used)
		return (len);
	return (used + (size_t)rc);
}

static void
virtio_external_send_transport_desc(cnc_conn_t c, int req_id,
    const struct virtio_external_backend *backend)
{
	const struct scorpi_virtio_external_transport_desc *transport =
	    &backend->transport;
	char response[VIRTIO_EXT_RESPONSE_MAX];
	size_t used = 0;
	int rc;

	rc = snprintf(response, sizeof(response),
	    "{\"accepted\":true,\"backend_id\":\"%s\",\"device_name\":\"%s\","
	    "\"protocol\":\"%s\",\"transport_ready\":%s,"
	    "\"features\":%llu,\"reset_generation\":%u,\"queues\":[",
	    backend->backend_id, backend->device_name, backend->protocol,
	    transport->ready ? "true" : "false",
	    (unsigned long long)transport->features,
	    transport->reset_generation);
	if (rc < 0 || (size_t)rc >= sizeof(response)) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"response_too_large\"}");
		return;
	}
	used = (size_t)rc;

	for (uint32_t i = 0; i < transport->queue_count; i++)
		used = virtio_external_append_queue_json(response,
		    sizeof(response), used, &transport->queues[i]);
	if (used >= sizeof(response))
		goto too_large;

	rc = snprintf(response + used, sizeof(response) - used,
	    "],\"memory_regions\":[");
	if (rc < 0 || (size_t)rc >= sizeof(response) - used)
		goto too_large;
	used += (size_t)rc;

	for (uint32_t i = 0; i < transport->memory_region_count; i++)
		used = virtio_external_append_memory_json(response,
		    sizeof(response), used, &transport->memory_regions[i]);
	if (used >= sizeof(response))
		goto too_large;

	rc = snprintf(response + used, sizeof(response) - used, "]}");
	if (rc < 0 || (size_t)rc >= sizeof(response) - used)
		goto too_large;

	cnc_send_response(c, req_id, response);
	return;

too_large:
	cnc_send_response(c, req_id,
	    "{\"accepted\":false,\"reason\":\"response_too_large\"}");
}

static void
virtio_device_describe(cnc_conn_t c, int req_id, int argc, char *argv[],
    void *param)
{
	struct virtio_external_backend *backend;
	struct virtio_external_backend snapshot;

	(void)param;
	if (argc < 1) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_args\"}");
		return;
	}
	if (!virtio_external_valid_name(argv[0])) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_name\"}");
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
	snapshot = *backend;
	pthread_mutex_unlock(&backends_lock);
	virtio_external_send_transport_desc(c, req_id, &snapshot);
}

static void
virtio_queue_kick(cnc_conn_t c, int req_id, int argc, char *argv[], void *param)
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
	struct virtio_external_backend *backend;
	virtio_external_interrupt_cb interrupt_cb;
	void *opaque;
	uint32_t queue_index;

	(void)param;
	if (argc < 2) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_args\"}");
		return;
	}
	if (!virtio_external_valid_name(argv[0])) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_name\"}");
		return;
	}

	queue_index = (uint32_t)strtoul(argv[1], NULL, 10);
	pthread_mutex_lock(&backends_lock);
	backend = virtio_external_backend_find_locked(argv[0]);
	if (backend == NULL || backend->interrupt_cb == NULL) {
		pthread_mutex_unlock(&backends_lock);
		virtio_external_backend_send_not_ready(c, req_id);
		return;
	}
	interrupt_cb = backend->interrupt_cb;
	opaque = backend->device_opaque;
	pthread_mutex_unlock(&backends_lock);

	interrupt_cb(opaque, queue_index);
	cnc_send_response(c, req_id, "{\"accepted\":true}");
}

static void
virtio_device_reset(cnc_conn_t c, int req_id, int argc, char *argv[],
    void *param)
{
	struct virtio_external_backend *backend;
	virtio_external_reset_cb reset_cb;
	void *opaque;

	(void)param;
	if (argc < 1) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_args\"}");
		return;
	}
	if (!virtio_external_valid_name(argv[0])) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_name\"}");
		return;
	}

	pthread_mutex_lock(&backends_lock);
	backend = virtio_external_backend_find_locked(argv[0]);
	if (backend == NULL || backend->reset_cb == NULL) {
		pthread_mutex_unlock(&backends_lock);
		virtio_external_backend_send_not_ready(c, req_id);
		return;
	}
	reset_cb = backend->reset_cb;
	opaque = backend->device_opaque;
	pthread_mutex_unlock(&backends_lock);

	reset_cb(opaque);
	cnc_send_response(c, req_id, "{\"accepted\":true}");
}

void
virtio_external_backend_init(void)
{
	static int once;

	if (once)
		return;

	cnc_register_command("virtio_backend_register", virtio_backend_register,
	    NULL);
	cnc_register_command("virtio_device_describe", virtio_device_describe,
	    NULL);
	cnc_register_command("virtio_queue_kick", virtio_queue_kick, NULL);
	cnc_register_command("virtio_queue_interrupt", virtio_queue_interrupt,
	    NULL);
	cnc_register_command("virtio_device_reset", virtio_device_reset, NULL);
	cnc_register_command("virtio_backend_disconnect",
	    virtio_backend_disconnect, NULL);
	once = 1;
}
