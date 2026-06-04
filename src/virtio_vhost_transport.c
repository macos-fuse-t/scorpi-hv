/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/queue.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cnc.h"
#include "debug.h"
#include "virtio_vhost_transport.h"

#define VIRTIO_VHOST_RESPONSE_MAX	  (64 * 1024)
#define VIRTIO_VHOST_SOCKET_PATH_MAX	  PATH_MAX
#define VIRTIO_VHOST_CONNECT_BATCH_MAX	  16
#define VIRTIO_VHOST_REGISTER_TIMEOUT_SEC 10

struct virtio_vhost_transport {
	char backend_id[SCORPI_VIRTIO_VHOST_NAME_MAX];
	char device_name[SCORPI_VIRTIO_VHOST_NAME_MAX];
	char protocol[SCORPI_VIRTIO_VHOST_NAME_MAX];
	char socket_path[VIRTIO_VHOST_SOCKET_PATH_MAX];
	cnc_conn_t conn;
	bool connected;
	bool connection_started;
	bool device_features_valid;
	bool device_features_applied;
	uint64_t device_features;
	struct scorpi_virtio_vhost_transport_desc transport;
	virtio_vhost_interrupt_cb interrupt_cb;
	virtio_vhost_reset_cb reset_cb;
	virtio_vhost_device_features_cb device_features_cb;
	void *device_opaque;
	LIST_ENTRY(virtio_vhost_transport) entries;
};

static LIST_HEAD(virtio_vhost_transports,
    virtio_vhost_transport) backends = LIST_HEAD_INITIALIZER(backends);
static pthread_mutex_t backends_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t backends_cond = PTHREAD_COND_INITIALIZER;

static size_t virtio_vhost_transport_append_queue_json(char *buf, size_t len,
    size_t used, const struct scorpi_virtio_vhost_queue_desc *queue);
static size_t virtio_vhost_transport_append_memory_json(char *buf, size_t len,
    size_t used, const struct scorpi_virtio_vhost_memory_region_desc *region);

static struct virtio_vhost_transport *
virtio_vhost_transport_find_locked(const char *backend_id)
{
	struct virtio_vhost_transport *backend;

	LIST_FOREACH(backend, &backends, entries) {
		if (strcmp(backend->backend_id, backend_id) == 0)
			return (backend);
	}
	return (NULL);
}

static bool
virtio_vhost_transport_valid_name(const char *name)
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
virtio_vhost_transport_registered(const char *backend_id)
{
	bool registered;

	pthread_mutex_lock(&backends_lock);
	registered = virtio_vhost_transport_find_locked(backend_id) != NULL;
	pthread_mutex_unlock(&backends_lock);
	return (registered);
}

static bool
virtio_vhost_transport_socket_path_valid(const char *socket_path)
{
	return (socket_path != NULL && socket_path[0] != '\0' &&
	    strlen(socket_path) < VIRTIO_VHOST_SOCKET_PATH_MAX);
}

static bool
virtio_vhost_parse_u64(const char *value, uint64_t *out)
{
	char *end;
	unsigned long long parsed;

	if (value == NULL || value[0] == '\0')
		return (false);
	errno = 0;
	parsed = strtoull(value, &end, 0);
	if (errno != 0 || end == value || *end != '\0')
		return (false);
	*out = (uint64_t)parsed;
	return (true);
}

int
virtio_vhost_transport_set_backend_socket(const char *backend_id,
    const char *socket_path)
{
	struct virtio_vhost_transport *backend;
	int rc = 0;

	if (!virtio_vhost_transport_valid_name(backend_id) ||
	    !virtio_vhost_transport_socket_path_valid(socket_path))
		return (-1);

	pthread_mutex_lock(&backends_lock);
	backend = virtio_vhost_transport_find_locked(backend_id);
	if (backend == NULL)
		rc = -1;
	else
		snprintf(backend->socket_path, sizeof(backend->socket_path),
		    "%s", socket_path);
	pthread_mutex_unlock(&backends_lock);
	return (rc);
}

int
virtio_vhost_transport_connect_backend(const char *backend_id,
    const char *socket_path)
{
	if (backend_id == NULL || socket_path == NULL)
		return (-1);
	if (!virtio_vhost_transport_valid_name(backend_id) ||
	    !virtio_vhost_transport_socket_path_valid(socket_path))
		return (-1);
	if (!virtio_vhost_transport_registered(backend_id))
		return (-1);

	virtio_vhost_transport_init();
	PRINTLN("connecting virtio vhost backend %s at %s", backend_id,
	    socket_path);
	return (cnc_connect_client(socket_path));
}

int
virtio_vhost_transport_connect_configured_backends(void)
{
	struct pending_backend {
		char backend_id[SCORPI_VIRTIO_VHOST_NAME_MAX];
		char socket_path[VIRTIO_VHOST_SOCKET_PATH_MAX];
	} pending[VIRTIO_VHOST_CONNECT_BATCH_MAX];
	struct virtio_vhost_transport *backend;
	size_t count = 0;
	int rc = 0;

	virtio_vhost_transport_init();

	pthread_mutex_lock(&backends_lock);
	LIST_FOREACH(backend, &backends, entries) {
		if (backend->socket_path[0] == '\0' ||
		    backend->connection_started)
			continue;
		if (count >= VIRTIO_VHOST_CONNECT_BATCH_MAX) {
			rc = -1;
			break;
		}
		snprintf(pending[count].backend_id,
		    sizeof(pending[count].backend_id), "%s",
		    backend->backend_id);
		snprintf(pending[count].socket_path,
		    sizeof(pending[count].socket_path), "%s",
		    backend->socket_path);
		backend->connection_started = true;
		count++;
	}
	pthread_mutex_unlock(&backends_lock);

	if (rc != 0)
		return (rc);

	for (size_t i = 0; i < count; i++) {
		if (virtio_vhost_transport_connect_backend(
			pending[i].backend_id, pending[i].socket_path) != 0)
			return (-1);
	}
	return (0);
}

int
virtio_vhost_transport_wait_configured_backends_registered(void)
{
	struct virtio_vhost_transport *backend;
	struct timespec deadline;
	int rc = 0;

	if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
		return (-1);
	deadline.tv_sec += VIRTIO_VHOST_REGISTER_TIMEOUT_SEC;

	pthread_mutex_lock(&backends_lock);
	for (;;) {
		const char *waiting_backend = NULL;

		LIST_FOREACH(backend, &backends, entries) {
			if (backend->socket_path[0] == '\0')
				continue;
			if (backend->connected &&
			    backend->device_features_valid &&
			    backend->device_features_applied)
				continue;
			waiting_backend = backend->backend_id;
			break;
		}
		if (waiting_backend == NULL)
			break;

		rc = pthread_cond_timedwait(&backends_cond, &backends_lock,
		    &deadline);
		if (rc == ETIMEDOUT) {
			EPRINTLN(
			    "timed out waiting for virtio vhost backend %s to register",
			    waiting_backend);
			rc = -1;
			break;
		}
		if (rc != 0) {
			EPRINTLN(
			    "failed waiting for virtio vhost backend registration: %s",
			    strerror(rc));
			rc = -1;
			break;
		}
	}
	pthread_mutex_unlock(&backends_lock);
	return (rc);
}

int
virtio_vhost_transport_set_transport(const char *backend_id,
    const struct scorpi_virtio_vhost_transport_desc *transport)
{
	struct virtio_vhost_transport *backend;
	int rc = 0;

	if (backend_id == NULL || transport == NULL)
		return (-1);
	if (transport->queue_count > SCORPI_VIRTIO_VHOST_MAX_QUEUES ||
	    transport->memory_region_count >
		SCORPI_VIRTIO_VHOST_MAX_MEMORY_REGIONS)
		return (-1);

	pthread_mutex_lock(&backends_lock);
	backend = virtio_vhost_transport_find_locked(backend_id);
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
	pthread_cond_broadcast(&backends_cond);
done:
	pthread_mutex_unlock(&backends_lock);
	return (rc);
}

int
virtio_vhost_transport_bind_device(const char *backend_id,
    const struct scorpi_virtio_vhost_transport_desc *transport,
    virtio_vhost_interrupt_cb interrupt_cb, virtio_vhost_reset_cb reset_cb,
    virtio_vhost_device_features_cb device_features_cb, void *opaque)
{
	struct virtio_vhost_transport *backend;
	virtio_vhost_device_features_cb applied_features_cb = NULL;
	void *applied_features_opaque = NULL;
	uint64_t device_features = 0;
	bool device_features_valid = false;
	int rc;

	rc = virtio_vhost_transport_set_transport(backend_id, transport);
	if (rc != 0)
		return (rc);

	pthread_mutex_lock(&backends_lock);
	backend = virtio_vhost_transport_find_locked(backend_id);
	if (backend == NULL) {
		pthread_mutex_unlock(&backends_lock);
		return (-1);
	}
	backend->interrupt_cb = interrupt_cb;
	backend->reset_cb = reset_cb;
	backend->device_features_cb = device_features_cb;
	backend->device_opaque = opaque;
	if (backend->device_features_valid) {
		device_features = backend->device_features;
		device_features_valid = true;
		backend->device_features_applied = device_features_cb == NULL;
		applied_features_cb = device_features_cb;
		applied_features_opaque = opaque;
	}
	pthread_cond_broadcast(&backends_cond);
	pthread_mutex_unlock(&backends_lock);
	if (device_features_valid && applied_features_cb != NULL) {
		applied_features_cb(applied_features_opaque, device_features);
		pthread_mutex_lock(&backends_lock);
		backend = virtio_vhost_transport_find_locked(backend_id);
		if (backend != NULL) {
			backend->device_features_applied = true;
			pthread_cond_broadcast(&backends_cond);
		}
		pthread_mutex_unlock(&backends_lock);
	}
	return (0);
}

void
virtio_vhost_transport_clear_transport(const char *backend_id)
{
	struct virtio_vhost_transport *backend;

	pthread_mutex_lock(&backends_lock);
	backend = virtio_vhost_transport_find_locked(backend_id);
	if (backend != NULL)
		memset(&backend->transport, 0, sizeof(backend->transport));
	pthread_cond_broadcast(&backends_cond);
	pthread_mutex_unlock(&backends_lock);
}

int
virtio_vhost_transport_notify_queue_kick(const char *backend_id,
    uint32_t queue_index)
{
	struct virtio_vhost_transport *backend;
	cnc_conn_t conn;
	char notification[VIRTIO_VHOST_RESPONSE_MAX];
	size_t used;
	int rc;

	if (backend_id == NULL)
		return (-1);

	pthread_mutex_lock(&backends_lock);
	backend = virtio_vhost_transport_find_locked(backend_id);
	if (backend == NULL || !backend->connected || backend->conn == NULL) {
		pthread_mutex_unlock(&backends_lock);
		return (-1);
	}
	conn = backend->conn;

	rc = snprintf(notification, sizeof(notification),
	    "{ \"event\": \"%s\", \"data\": {"
	    "\"backend_id\": \"%s\","
	    "\"device_name\": \"%s\","
	    "\"queue_index\": %u,"
	    "\"reset_generation\": %u,"
	    "\"queues\":[",
	    SCORPI_VIRTIO_VHOST_EVENT_QUEUE_KICK, backend->backend_id,
	    backend->device_name, queue_index,
	    backend->transport.reset_generation);
	if (rc < 0 || (size_t)rc >= sizeof(notification))
		goto too_large;
	used = (size_t)rc;

	for (uint32_t i = 0; i < backend->transport.queue_count; i++)
		used = virtio_vhost_transport_append_queue_json(notification,
		    sizeof(notification), used, &backend->transport.queues[i]);
	if (used >= sizeof(notification))
		goto too_large;

	rc = snprintf(notification + used, sizeof(notification) - used,
	    "],\"memory_regions\":[");
	if (rc < 0 || (size_t)rc >= sizeof(notification) - used)
		goto too_large;
	used += (size_t)rc;

	for (uint32_t i = 0; i < backend->transport.memory_region_count; i++)
		used = virtio_vhost_transport_append_memory_json(notification,
		    sizeof(notification), used,
		    &backend->transport.memory_regions[i]);
	if (used >= sizeof(notification))
		goto too_large;

	rc = snprintf(notification + used, sizeof(notification) - used, "]} }");
	if (rc < 0 || (size_t)rc >= sizeof(notification) - used)
		goto too_large;
	pthread_mutex_unlock(&backends_lock);

	cnc_send_notification_to(conn, notification);
	return (0);

too_large:
	pthread_mutex_unlock(&backends_lock);
	return (-1);
}

int
virtio_vhost_transport_notify_display_resize(const char *backend_id,
    uint32_t width, uint32_t height)
{
	struct virtio_vhost_transport *backend;
	cnc_conn_t conn;
	char notification[VIRTIO_VHOST_RESPONSE_MAX];
	int rc;

	if (backend_id == NULL)
		return (-1);

	pthread_mutex_lock(&backends_lock);
	backend = virtio_vhost_transport_find_locked(backend_id);
	if (backend == NULL || !backend->connected || backend->conn == NULL) {
		pthread_mutex_unlock(&backends_lock);
		return (-1);
	}
	conn = backend->conn;

	rc = snprintf(notification, sizeof(notification),
	    "{ \"event\": \"%s\", \"data\": {"
	    "\"backend_id\": \"%s\","
	    "\"device_name\": \"%s\","
	    "\"width\": %u,"
	    "\"height\": %u,"
	    "\"reset_generation\": %u"
	    "} }",
	    SCORPI_VIRTIO_VHOST_EVENT_GPU_RESIZE, backend->backend_id,
	    backend->device_name, width, height,
	    backend->transport.reset_generation);
	if (rc < 0 || (size_t)rc >= sizeof(notification)) {
		pthread_mutex_unlock(&backends_lock);
		return (-1);
	}
	pthread_mutex_unlock(&backends_lock);

	cnc_send_notification_to(conn, notification);
	return (0);
}

static void
virtio_vhost_transport_send_not_ready(cnc_conn_t c, int req_id)
{
	cnc_send_response(c, req_id,
	    "{\"accepted\":false,\"reason\":\"not_ready\"}");
}

static void
virtio_vhost_register(cnc_conn_t c, int req_id, int argc, char *argv[],
    void *param)
{
	struct virtio_vhost_transport *backend;
	virtio_vhost_device_features_cb device_features_cb = NULL;
	void *device_opaque = NULL;
	uint64_t device_features = 0;

	(void)param;
	if (argc < 3) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_args\"}");
		return;
	}
	if (!virtio_vhost_transport_valid_name(argv[0]) ||
	    !virtio_vhost_transport_valid_name(argv[1]) ||
	    !virtio_vhost_transport_valid_name(argv[2])) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_name\"}");
		return;
	}
	if (argc >= 4) {
		if (!virtio_vhost_parse_u64(argv[3], &device_features)) {
			cnc_send_response(c, req_id,
			    "{\"accepted\":false,\"reason\":\"bad_features\"}");
			return;
		}
	}

	pthread_mutex_lock(&backends_lock);
	backend = virtio_vhost_transport_find_locked(argv[0]);
	if (backend != NULL) {
		backend->conn = c;
		backend->connected = true;
		backend->device_features = device_features;
		backend->device_features_valid = true;
		backend->device_features_applied = false;
		snprintf(backend->device_name, sizeof(backend->device_name),
		    "%s", argv[1]);
		snprintf(backend->protocol, sizeof(backend->protocol), "%s",
		    argv[2]);
		snprintf(backend->transport.backend_id,
		    sizeof(backend->transport.backend_id), "%s", argv[0]);
		snprintf(backend->transport.device_name,
		    sizeof(backend->transport.device_name), "%s", argv[1]);
		device_features_cb = backend->device_features_cb;
		device_opaque = backend->device_opaque;
		if (device_features_cb == NULL) {
			backend->device_features_applied = true;
			pthread_cond_broadcast(&backends_cond);
		}
		pthread_mutex_unlock(&backends_lock);
		if (device_features_cb != NULL) {
			device_features_cb(device_opaque, device_features);
			pthread_mutex_lock(&backends_lock);
			backend = virtio_vhost_transport_find_locked(argv[0]);
			if (backend != NULL) {
				backend->device_features_applied = true;
				pthread_cond_broadcast(&backends_cond);
			}
			pthread_mutex_unlock(&backends_lock);
		}
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
	backend->device_features = device_features;
	backend->device_features_valid = true;
	backend->device_features_applied = true;
	snprintf(backend->transport.backend_id,
	    sizeof(backend->transport.backend_id), "%s", argv[0]);
	snprintf(backend->transport.device_name,
	    sizeof(backend->transport.device_name), "%s", argv[1]);
	LIST_INSERT_HEAD(&backends, backend, entries);
	pthread_cond_broadcast(&backends_cond);
	pthread_mutex_unlock(&backends_lock);

	PRINTLN("virtio vhost backend registered: id=%s device=%s protocol=%s",
	    backend->backend_id, backend->device_name, backend->protocol);
	cnc_send_response(c, req_id, "{\"accepted\":true,\"updated\":false}");
}

static void
virtio_vhost_disconnect(cnc_conn_t c, int req_id, int argc, char *argv[],
    void *param)
{
	struct virtio_vhost_transport *backend;

	(void)param;
	if (argc < 1) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_args\"}");
		return;
	}
	if (!virtio_vhost_transport_valid_name(argv[0])) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_name\"}");
		return;
	}

	pthread_mutex_lock(&backends_lock);
	backend = virtio_vhost_transport_find_locked(argv[0]);
	if (backend == NULL) {
		pthread_mutex_unlock(&backends_lock);
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"not_registered\"}");
		return;
	}

	LIST_REMOVE(backend, entries);
	PRINTLN("virtio vhost backend disconnected: id=%s",
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
	pthread_cond_broadcast(&backends_cond);
	pthread_mutex_unlock(&backends_lock);
	cnc_send_response(c, req_id, "{\"accepted\":true}");
}

static size_t
virtio_vhost_transport_append_queue_json(char *buf, size_t len, size_t used,
    const struct scorpi_virtio_vhost_queue_desc *queue)
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
virtio_vhost_transport_append_memory_json(char *buf, size_t len, size_t used,
    const struct scorpi_virtio_vhost_memory_region_desc *region)
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
virtio_vhost_transport_send_transport_desc(cnc_conn_t c, int req_id,
    const struct virtio_vhost_transport *backend)
{
	const struct scorpi_virtio_vhost_transport_desc *transport =
	    &backend->transport;
	char response[VIRTIO_VHOST_RESPONSE_MAX];
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
		used = virtio_vhost_transport_append_queue_json(response,
		    sizeof(response), used, &transport->queues[i]);
	if (used >= sizeof(response))
		goto too_large;

	rc = snprintf(response + used, sizeof(response) - used,
	    "],\"memory_regions\":[");
	if (rc < 0 || (size_t)rc >= sizeof(response) - used)
		goto too_large;
	used += (size_t)rc;

	for (uint32_t i = 0; i < transport->memory_region_count; i++)
		used = virtio_vhost_transport_append_memory_json(response,
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
virtio_vhost_describe(cnc_conn_t c, int req_id, int argc, char *argv[],
    void *param)
{
	struct virtio_vhost_transport *backend;
	struct virtio_vhost_transport snapshot;

	(void)param;
	if (argc < 1) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_args\"}");
		return;
	}
	if (!virtio_vhost_transport_valid_name(argv[0])) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_name\"}");
		return;
	}

	pthread_mutex_lock(&backends_lock);
	backend = virtio_vhost_transport_find_locked(argv[0]);
	if (backend == NULL) {
		pthread_mutex_unlock(&backends_lock);
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"not_registered\"}");
		return;
	}
	snapshot = *backend;
	pthread_mutex_unlock(&backends_lock);
	virtio_vhost_transport_send_transport_desc(c, req_id, &snapshot);
}

static void
virtio_vhost_queue_kick(cnc_conn_t c, int req_id, int argc, char *argv[],
    void *param)
{
	(void)argc;
	(void)argv;
	(void)param;
	virtio_vhost_transport_send_not_ready(c, req_id);
}

static void
virtio_vhost_queue_interrupt(cnc_conn_t c, int req_id, int argc, char *argv[],
    void *param)
{
	struct virtio_vhost_transport *backend;
	virtio_vhost_interrupt_cb interrupt_cb;
	void *opaque;
	uint32_t queue_index;

	(void)param;
	if (argc < 2) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_args\"}");
		return;
	}
	if (!virtio_vhost_transport_valid_name(argv[0])) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_name\"}");
		return;
	}

	queue_index = (uint32_t)strtoul(argv[1], NULL, 10);
	pthread_mutex_lock(&backends_lock);
	backend = virtio_vhost_transport_find_locked(argv[0]);
	if (backend == NULL || backend->interrupt_cb == NULL) {
		pthread_mutex_unlock(&backends_lock);
		virtio_vhost_transport_send_not_ready(c, req_id);
		return;
	}
	interrupt_cb = backend->interrupt_cb;
	opaque = backend->device_opaque;
	pthread_mutex_unlock(&backends_lock);

	interrupt_cb(opaque, queue_index);
}

static void
virtio_vhost_reset(cnc_conn_t c, int req_id, int argc, char *argv[],
    void *param)
{
	struct virtio_vhost_transport *backend;
	virtio_vhost_reset_cb reset_cb;
	void *opaque;

	(void)param;
	if (argc < 1) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_args\"}");
		return;
	}
	if (!virtio_vhost_transport_valid_name(argv[0])) {
		cnc_send_response(c, req_id,
		    "{\"accepted\":false,\"reason\":\"bad_name\"}");
		return;
	}

	pthread_mutex_lock(&backends_lock);
	backend = virtio_vhost_transport_find_locked(argv[0]);
	if (backend == NULL || backend->reset_cb == NULL) {
		pthread_mutex_unlock(&backends_lock);
		virtio_vhost_transport_send_not_ready(c, req_id);
		return;
	}
	reset_cb = backend->reset_cb;
	opaque = backend->device_opaque;
	pthread_mutex_unlock(&backends_lock);

	reset_cb(opaque);
	cnc_send_response(c, req_id, "{\"accepted\":true}");
}

void
virtio_vhost_transport_init(void)
{
	static int once;

	if (once)
		return;

	cnc_register_command(SCORPI_VIRTIO_VHOST_CMD_REGISTER,
	    virtio_vhost_register, NULL);
	cnc_register_command(SCORPI_VIRTIO_VHOST_CMD_DESCRIBE,
	    virtio_vhost_describe, NULL);
	cnc_register_command(SCORPI_VIRTIO_VHOST_CMD_QUEUE_KICK,
	    virtio_vhost_queue_kick, NULL);
	cnc_register_command(SCORPI_VIRTIO_VHOST_CMD_QUEUE_INTERRUPT,
	    virtio_vhost_queue_interrupt, NULL);
	cnc_register_command(SCORPI_VIRTIO_VHOST_CMD_RESET, virtio_vhost_reset,
	    NULL);
	cnc_register_command(SCORPI_VIRTIO_VHOST_CMD_DISCONNECT,
	    virtio_vhost_disconnect, NULL);
	once = 1;
}
