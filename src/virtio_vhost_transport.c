/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/mman.h>
#include <sys/queue.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <scorpi/vhost_user.h>

#include "debug.h"
#include "mevent.h"
#include "virtio_vhost_transport.h"

#define VIRTIO_VHOST_SOCKET_PATH_MAX	  PATH_MAX
#define VIRTIO_VHOST_CONNECT_BATCH_MAX	  16
#define VIRTIO_VHOST_REGISTER_TIMEOUT_SEC 10
#define VIRTIO_VHOST_NOTIFY_BYTE	  1

struct virtio_vhost_call_event {
	struct virtio_vhost_transport *backend;
	uint32_t queue_index;
};

struct virtio_vhost_transport {
	char backend_id[SCORPI_VIRTIO_VHOST_NAME_MAX];
	char device_name[SCORPI_VIRTIO_VHOST_NAME_MAX];
	char socket_path[VIRTIO_VHOST_SOCKET_PATH_MAX];
	int fd;
	pthread_mutex_t io_mtx;
	int kick_read_fds[SCORPI_VIRTIO_VHOST_MAX_QUEUES];
	int kick_write_fds[SCORPI_VIRTIO_VHOST_MAX_QUEUES];
	int call_read_fds[SCORPI_VIRTIO_VHOST_MAX_QUEUES];
	int call_write_fds[SCORPI_VIRTIO_VHOST_MAX_QUEUES];
	struct mevent *call_events[SCORPI_VIRTIO_VHOST_MAX_QUEUES];
	struct virtio_vhost_call_event
	    call_event_params[SCORPI_VIRTIO_VHOST_MAX_QUEUES];
	bool notify_fds_sent[SCORPI_VIRTIO_VHOST_MAX_QUEUES];
	bool connected;
	bool connection_started;
	bool setup_dirty;
	bool device_features_valid;
	bool device_features_applied;
	uint32_t sent_reset_generation;
	uint64_t device_features;
	struct scorpi_virtio_vhost_transport_desc transport;
	virtio_vhost_interrupt_cb interrupt_cb;
	virtio_vhost_device_features_cb device_features_cb;
	virtio_vhost_event_cb event_cb;
	void *device_opaque;
	LIST_ENTRY(virtio_vhost_transport) entries;
};

static LIST_HEAD(virtio_vhost_transports,
    virtio_vhost_transport) backends = LIST_HEAD_INITIALIZER(backends);
static pthread_mutex_t backends_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t backends_cond = PTHREAD_COND_INITIALIZER;

static void virtio_vhost_transport_call_event(int fd, enum ev_type type,
    void *param);
static void virtio_vhost_transport_handle_backend_event(
    struct virtio_vhost_transport *backend, const struct scorpi_vhost_msg *msg);
static void virtio_vhost_transport_recv_pending_events(
    struct virtio_vhost_transport *backend);

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

static void
virtio_vhost_transport_close_fd(struct virtio_vhost_transport *backend)
{
	if (backend->fd >= 0) {
		close(backend->fd);
		backend->fd = -1;
	}
}

static int
virtio_vhost_transport_set_nonblock(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return (-1);
	return (fcntl(fd, F_SETFL, flags | O_NONBLOCK));
}

static void
virtio_vhost_transport_close_int(int *fdp)
{
	if (*fdp >= 0) {
		close(*fdp);
		*fdp = -1;
	}
}

static void
virtio_vhost_transport_close_queue_notify_fds(
    struct virtio_vhost_transport *backend, size_t queue_index)
{
	if (backend->call_events[queue_index] != NULL) {
		mevent_delete_close(backend->call_events[queue_index]);
		backend->call_events[queue_index] = NULL;
		backend->call_read_fds[queue_index] = -1;
	}
	virtio_vhost_transport_close_int(&backend->kick_read_fds[queue_index]);
	virtio_vhost_transport_close_int(&backend->kick_write_fds[queue_index]);
	virtio_vhost_transport_close_int(&backend->call_read_fds[queue_index]);
	virtio_vhost_transport_close_int(&backend->call_write_fds[queue_index]);
	backend->notify_fds_sent[queue_index] = false;
}

static void
virtio_vhost_transport_close_notify_fds(struct virtio_vhost_transport *backend)
{
	for (size_t i = 0; i < SCORPI_VIRTIO_VHOST_MAX_QUEUES; i++) {
		virtio_vhost_transport_close_queue_notify_fds(backend, i);
	}
}

static int
virtio_vhost_transport_init_backend(struct virtio_vhost_transport *backend)
{
	backend->fd = -1;
	for (size_t i = 0; i < SCORPI_VIRTIO_VHOST_MAX_QUEUES; i++) {
		backend->kick_read_fds[i] = -1;
		backend->kick_write_fds[i] = -1;
		backend->call_read_fds[i] = -1;
		backend->call_write_fds[i] = -1;
		backend->call_event_params[i].backend = backend;
		backend->call_event_params[i].queue_index = (uint32_t)i;
	}
	pthread_mutex_init(&backend->io_mtx, NULL);
	return (0);
}

static int
virtio_vhost_transport_recv_reply(struct virtio_vhost_transport *backend,
    int fd, uint32_t request, struct scorpi_vhost_msg *reply)
{
	int fds[VHOST_USER_MAX_FDS];
	ssize_t nread;

	for (;;) {
		size_t fd_count = 0;

		for (size_t i = 0; i < VHOST_USER_MAX_FDS; i++)
			fds[i] = -1;
		nread = vhost_user_recv_msg(fd, reply, fds, VHOST_USER_MAX_FDS,
		    &fd_count);
		for (size_t i = 0; i < fd_count; i++) {
			if (fds[i] >= 0)
				close(fds[i]);
		}
		if (nread <= 0)
			return (-1);

		if ((reply->header.flags & SCORPI_VHOST_FLAG_REPLY) != 0) {
			if (reply->header.request != request)
				return (-1);
			return (0);
		}

		if (backend == NULL)
			return (-1);
		virtio_vhost_transport_handle_backend_event(backend, reply);
	}
}

static int
virtio_vhost_transport_send_request_fds(
    struct virtio_vhost_transport *backend, int fd,
    struct scorpi_vhost_msg *msg, const int *fds, size_t fd_count,
    struct scorpi_vhost_msg *reply)
{
	msg->header.flags |= SCORPI_VHOST_FLAG_NEED_REPLY;
	if (vhost_user_send_msg(fd, msg, fds, fd_count) < 0)
		return (-1);
	return (virtio_vhost_transport_recv_reply(backend, fd,
	    msg->header.request, reply));
}

static int
virtio_vhost_transport_send_request(struct virtio_vhost_transport *backend,
    int fd, struct scorpi_vhost_msg *msg, struct scorpi_vhost_msg *reply)
{
	return (virtio_vhost_transport_send_request_fds(backend, fd, msg, NULL,
	    0, reply));
}

static int
virtio_vhost_transport_query_features(int fd, uint64_t *features)
{
	struct scorpi_vhost_msg request;
	struct scorpi_vhost_msg reply;

	vhost_user_msg_init(&request, SCORPI_VHOST_MSG_HELLO,
	    sizeof(request.payload.hello));
	request.payload.hello.frontend_features = 0;
	if (virtio_vhost_transport_send_request(NULL, fd, &request, &reply) !=
	    0)
		return (-1);

	vhost_user_msg_init(&request, SCORPI_VHOST_MSG_FEATURES, 0);
	if (virtio_vhost_transport_send_request(NULL, fd, &request, &reply) !=
	    0)
		return (-1);
	if (reply.header.size != sizeof(reply.payload.u64))
		return (-1);
	*features = reply.payload.u64;
	return (0);
}

static int
virtio_vhost_transport_send_u64(struct virtio_vhost_transport *backend,
    int fd, uint32_t request, uint64_t value)
{
	struct scorpi_vhost_msg msg;
	struct scorpi_vhost_msg reply;

	vhost_user_msg_init(&msg, request, sizeof(msg.payload.u64));
	msg.payload.u64 = value;
	return (virtio_vhost_transport_send_request(backend, fd, &msg, &reply));
}

static int
virtio_vhost_transport_send_queue_state(
    struct virtio_vhost_transport *backend, int fd, uint32_t request,
    uint32_t index, uint32_t num)
{
	struct scorpi_vhost_msg msg;
	struct scorpi_vhost_msg reply;

	vhost_user_msg_init(&msg, request, sizeof(msg.payload.queue_state));
	msg.payload.queue_state.index = index;
	msg.payload.queue_state.num = num;
	return (virtio_vhost_transport_send_request(backend, fd, &msg, &reply));
}

static int
virtio_vhost_transport_send_queue_fd(struct virtio_vhost_transport *backend,
    int fd, uint32_t request, uint32_t queue_index, int send_fd)
{
	struct scorpi_vhost_msg msg;
	struct scorpi_vhost_msg reply;

	vhost_user_msg_init(&msg, request, sizeof(msg.payload.u64));
	msg.payload.u64 = queue_index;
	return (virtio_vhost_transport_send_request_fds(backend, fd, &msg,
	    &send_fd, 1, &reply));
}

static void
virtio_vhost_transport_close_fds(int *fds, size_t fd_count)
{
	for (size_t i = 0; i < fd_count; i++) {
		if (fds[i] >= 0) {
			close(fds[i]);
			fds[i] = -1;
		}
	}
}

static int
virtio_vhost_transport_open_memory_fds(
    const struct scorpi_virtio_vhost_transport_desc *transport, int *fds,
    size_t *fd_count)
{
	for (uint32_t i = 0; i < transport->memory_region_count; i++) {
		if (i >= VHOST_USER_MAX_FDS)
			return (-1);
		fds[i] = shm_open(transport->memory_regions[i].shm_name, O_RDWR,
		    0);
		if (fds[i] < 0) {
			EPRINTLN("virtio vhost: shm_open %s: %s",
			    transport->memory_regions[i].shm_name,
			    strerror(errno));
			*fd_count = i;
			virtio_vhost_transport_close_fds(fds, *fd_count);
			return (-1);
		}
	}
	*fd_count = transport->memory_region_count;
	return (0);
}

static void
virtio_vhost_transport_drain_notify_fd(int fd)
{
	uint8_t buf[64];

	for (;;) {
		ssize_t nread;

		nread = read(fd, buf, sizeof(buf));
		if (nread > 0)
			continue;
		if (nread < 0 && errno == EINTR)
			continue;
		if (nread < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			EPRINTLN("virtio vhost: failed to drain notify fd: %s",
			    strerror(errno));
		break;
	}
}

static void
virtio_vhost_transport_call_event(int fd, enum ev_type type, void *param)
{
	struct virtio_vhost_call_event *event = param;
	struct virtio_vhost_transport *backend = event->backend;
	virtio_vhost_interrupt_cb interrupt_cb;
	void *opaque;

	(void)type;
	virtio_vhost_transport_drain_notify_fd(fd);
	virtio_vhost_transport_recv_pending_events(backend);

	pthread_mutex_lock(&backends_lock);
	interrupt_cb = backend->interrupt_cb;
	opaque = backend->device_opaque;
	pthread_mutex_unlock(&backends_lock);
	if (interrupt_cb != NULL)
		interrupt_cb(opaque, event->queue_index);
}

static bool
virtio_vhost_transport_fd_readable(int fd)
{
	struct pollfd pfd;
	int rc;

	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	do {
		rc = poll(&pfd, 1, 0);
	} while (rc < 0 && errno == EINTR);
	if (rc <= 0)
		return (false);
	if ((pfd.revents & POLLIN) == 0)
		return (false);
	return (true);
}

static void
virtio_vhost_transport_close_recv_fds(int *fds, size_t fd_count)
{
	for (size_t i = 0; i < fd_count; i++) {
		if (fds[i] >= 0)
			close(fds[i]);
	}
}

static void
virtio_vhost_transport_handle_backend_event(
    struct virtio_vhost_transport *backend, const struct scorpi_vhost_msg *msg)
{
	virtio_vhost_event_cb event_cb;
	void *opaque;

	if ((msg->header.flags & SCORPI_VHOST_FLAG_REPLY) != 0)
		return;

	pthread_mutex_lock(&backends_lock);
	event_cb = backend->event_cb;
	opaque = backend->device_opaque;
	pthread_mutex_unlock(&backends_lock);

	if (event_cb == NULL)
		return;
	event_cb(opaque, msg);
}

static void
virtio_vhost_transport_recv_pending_events(
    struct virtio_vhost_transport *backend)
{
	struct scorpi_vhost_msg msg;
	int fds[VHOST_USER_MAX_FDS];
	size_t fd_count;
	ssize_t nread;
	int fd;

	pthread_mutex_lock(&backends_lock);
	fd = backend->fd;
	pthread_mutex_unlock(&backends_lock);
	if (fd < 0)
		return;

	for (;;) {
		pthread_mutex_lock(&backend->io_mtx);
		if (!virtio_vhost_transport_fd_readable(fd)) {
			pthread_mutex_unlock(&backend->io_mtx);
			break;
		}
		for (size_t i = 0; i < VHOST_USER_MAX_FDS; i++)
			fds[i] = -1;
		fd_count = 0;
		nread = vhost_user_recv_msg(fd, &msg, fds, VHOST_USER_MAX_FDS,
		    &fd_count);
		virtio_vhost_transport_close_recv_fds(fds, fd_count);
		pthread_mutex_unlock(&backend->io_mtx);
		if (nread <= 0)
			break;
		virtio_vhost_transport_handle_backend_event(backend, &msg);
	}
}

static int
virtio_vhost_transport_send_memory_table(
    struct virtio_vhost_transport *backend, int fd,
    const struct scorpi_virtio_vhost_transport_desc *transport)
{
	struct scorpi_vhost_msg msg;
	struct scorpi_vhost_msg reply;
	int fds[VHOST_USER_MAX_FDS];
	size_t fd_count = 0;
	int rc;

	if (transport->memory_region_count > SCORPI_VHOST_MAX_MEMORY_REGIONS ||
	    transport->memory_region_count > VHOST_USER_MAX_FDS)
		return (-1);

	for (size_t i = 0; i < VHOST_USER_MAX_FDS; i++)
		fds[i] = -1;
	if (virtio_vhost_transport_open_memory_fds(transport, fds, &fd_count) !=
	    0)
		return (-1);

	vhost_user_msg_init(&msg, SCORPI_VHOST_MSG_SET_MEM_TABLE,
	    sizeof(msg.payload.memory));
	msg.payload.memory.nregions = transport->memory_region_count;
	for (uint32_t i = 0; i < transport->memory_region_count; i++) {
		msg.payload.memory.regions[i].guest_phys_addr =
		    transport->memory_regions[i].guest_phys_addr;
		msg.payload.memory.regions[i].memory_size =
		    transport->memory_regions[i].size;
		msg.payload.memory.regions[i].userspace_addr =
		    transport->memory_regions[i].guest_phys_addr;
		msg.payload.memory.regions[i].mmap_offset =
		    transport->memory_regions[i].shm_offset;
	}
	rc = virtio_vhost_transport_send_request_fds(backend, fd, &msg, fds,
	    fd_count, &reply);
	virtio_vhost_transport_close_fds(fds, fd_count);
	return (rc);
}

static int
virtio_vhost_transport_ensure_notify_fds_locked(
    struct virtio_vhost_transport *backend, uint32_t queue_index)
{
	int kick_fds[2] = { -1, -1 };
	int call_fds[2] = { -1, -1 };

	if (queue_index >= SCORPI_VIRTIO_VHOST_MAX_QUEUES)
		return (-1);
	if (backend->kick_write_fds[queue_index] >= 0 &&
	    backend->call_read_fds[queue_index] >= 0)
		return (0);

	virtio_vhost_transport_close_int(&backend->kick_read_fds[queue_index]);
	virtio_vhost_transport_close_int(&backend->kick_write_fds[queue_index]);
	virtio_vhost_transport_close_int(&backend->call_read_fds[queue_index]);
	virtio_vhost_transport_close_int(&backend->call_write_fds[queue_index]);
	backend->notify_fds_sent[queue_index] = false;

	if (vhost_user_make_notify_fds(kick_fds) != 0 ||
	    vhost_user_make_notify_fds(call_fds) != 0)
		goto fail;
	if (virtio_vhost_transport_set_nonblock(kick_fds[0]) != 0 ||
	    virtio_vhost_transport_set_nonblock(kick_fds[1]) != 0 ||
	    virtio_vhost_transport_set_nonblock(call_fds[0]) != 0 ||
	    virtio_vhost_transport_set_nonblock(call_fds[1]) != 0)
		goto fail;

	backend->kick_read_fds[queue_index] = kick_fds[0];
	backend->kick_write_fds[queue_index] = kick_fds[1];
	backend->call_read_fds[queue_index] = call_fds[0];
	backend->call_write_fds[queue_index] = call_fds[1];
	backend->call_events[queue_index] = mevent_add(call_fds[0], EVF_READ,
	    virtio_vhost_transport_call_event,
	    &backend->call_event_params[queue_index]);
	if (backend->call_events[queue_index] == NULL)
		goto fail_registered;
	return (0);

fail_registered:
	backend->kick_read_fds[queue_index] = -1;
	backend->kick_write_fds[queue_index] = -1;
	backend->call_read_fds[queue_index] = -1;
	backend->call_write_fds[queue_index] = -1;
fail:
	virtio_vhost_transport_close_int(&kick_fds[0]);
	virtio_vhost_transport_close_int(&kick_fds[1]);
	virtio_vhost_transport_close_int(&call_fds[0]);
	virtio_vhost_transport_close_int(&call_fds[1]);
	return (-1);
}

static int
virtio_vhost_transport_send_queue_notify_fds(
    struct virtio_vhost_transport *backend, int fd, uint32_t queue_index)
{
	int kick_fd;
	int call_fd;

	pthread_mutex_lock(&backends_lock);
	if (virtio_vhost_transport_ensure_notify_fds_locked(backend,
		queue_index) != 0) {
		pthread_mutex_unlock(&backends_lock);
		return (-1);
	}
	if (backend->notify_fds_sent[queue_index]) {
		pthread_mutex_unlock(&backends_lock);
		return (0);
	}
	kick_fd = backend->kick_read_fds[queue_index];
	call_fd = backend->call_write_fds[queue_index];
	pthread_mutex_unlock(&backends_lock);

	if (virtio_vhost_transport_send_queue_fd(backend, fd,
		SCORPI_VHOST_MSG_SET_QUEUE_KICK, queue_index, kick_fd) != 0) {
		pthread_mutex_lock(&backends_lock);
		virtio_vhost_transport_close_queue_notify_fds(backend,
		    queue_index);
		pthread_mutex_unlock(&backends_lock);
		return (-1);
	}

	pthread_mutex_lock(&backends_lock);
	if (backend->kick_read_fds[queue_index] == kick_fd)
		virtio_vhost_transport_close_int(
		    &backend->kick_read_fds[queue_index]);
	pthread_mutex_unlock(&backends_lock);

	if (virtio_vhost_transport_send_queue_fd(backend, fd,
		SCORPI_VHOST_MSG_SET_QUEUE_CALL, queue_index, call_fd) != 0) {
		pthread_mutex_lock(&backends_lock);
		virtio_vhost_transport_close_queue_notify_fds(backend,
		    queue_index);
		pthread_mutex_unlock(&backends_lock);
		return (-1);
	}

	pthread_mutex_lock(&backends_lock);
	if (backend->call_write_fds[queue_index] == call_fd)
		virtio_vhost_transport_close_int(
		    &backend->call_write_fds[queue_index]);
	backend->notify_fds_sent[queue_index] = true;
	pthread_mutex_unlock(&backends_lock);
	return (0);
}

static int
virtio_vhost_transport_send_queue_setup(struct virtio_vhost_transport *backend,
    int fd, const struct scorpi_virtio_vhost_queue_desc *queue)
{
	struct scorpi_vhost_msg msg;
	struct scorpi_vhost_msg reply;

	if (virtio_vhost_transport_send_queue_state(backend, fd,
		SCORPI_VHOST_MSG_SET_QUEUE_NUM, queue->index, queue->size) != 0)
		return (-1);

	vhost_user_msg_init(&msg, SCORPI_VHOST_MSG_SET_QUEUE_ADDR,
	    sizeof(msg.payload.queue_addr));
	msg.payload.queue_addr.index = queue->index;
	msg.payload.queue_addr.desc_user_addr = queue->desc_addr;
	msg.payload.queue_addr.avail_user_addr = queue->avail_addr;
	msg.payload.queue_addr.used_user_addr = queue->used_addr;
	if (virtio_vhost_transport_send_request(backend, fd, &msg, &reply) !=
	    0)
		return (-1);

	if (queue->ready && queue->size != 0 &&
	    virtio_vhost_transport_send_queue_notify_fds(backend, fd,
		queue->index) != 0)
		return (-1);

	return (virtio_vhost_transport_send_queue_state(backend, fd,
	    SCORPI_VHOST_MSG_SET_QUEUE_ENABLE, queue->index,
	    queue->ready ? 1 : 0));
}

static int
virtio_vhost_transport_send_reset_if_needed(
    struct virtio_vhost_transport *backend, int fd, uint32_t reset_generation)
{
	struct scorpi_vhost_msg msg;
	struct scorpi_vhost_msg reply;
	bool send_reset;

	pthread_mutex_lock(&backends_lock);
	send_reset = backend->sent_reset_generation != reset_generation;
	pthread_mutex_unlock(&backends_lock);
	if (!send_reset)
		return (0);

	PRINTLN("virtio vhost backend %s reset generation %u",
	    backend->backend_id, reset_generation);
	vhost_user_msg_init(&msg, SCORPI_VHOST_MSG_RESET_DEVICE, 0);
	if (virtio_vhost_transport_send_request(backend, fd, &msg, &reply) !=
	    0)
		return (-1);

	pthread_mutex_lock(&backends_lock);
	if (backend->sent_reset_generation != reset_generation) {
		virtio_vhost_transport_close_notify_fds(backend);
		backend->sent_reset_generation = reset_generation;
	}
	pthread_mutex_unlock(&backends_lock);
	return (0);
}

static int
virtio_vhost_transport_send_transport_setup(
    struct virtio_vhost_transport *backend, int fd,
    const struct scorpi_virtio_vhost_transport_desc *transport)
{
	if (virtio_vhost_transport_send_reset_if_needed(backend, fd,
		transport->reset_generation) != 0)
		return (-1);
	if (virtio_vhost_transport_send_u64(backend, fd,
		SCORPI_VHOST_MSG_SET_FEATURES, transport->features) != 0)
		return (-1);
	if (virtio_vhost_transport_send_memory_table(backend, fd, transport) !=
	    0)
		return (-1);
	for (uint32_t i = 0; i < transport->queue_count; i++) {
		if (!transport->queues[i].ready ||
		    transport->queues[i].size == 0)
			continue;
		if (virtio_vhost_transport_send_queue_setup(backend, fd,
			&transport->queues[i]) != 0)
			return (-1);
	}
	return (0);
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
virtio_vhost_transport_queue_ready(
    const struct scorpi_virtio_vhost_transport_desc *transport,
    uint32_t queue_index)
{
	for (uint32_t i = 0; i < transport->queue_count; i++) {
		const struct scorpi_virtio_vhost_queue_desc *queue =
		    &transport->queues[i];

		if (queue->index == queue_index)
			return (queue->ready && queue->size != 0);
	}
	return (false);
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
	struct virtio_vhost_transport *backend;
	virtio_vhost_device_features_cb device_features_cb = NULL;
	void *device_opaque = NULL;
	uint64_t device_features;
	int fd;

	if (backend_id == NULL || socket_path == NULL)
		return (-1);
	if (!virtio_vhost_transport_valid_name(backend_id) ||
	    !virtio_vhost_transport_socket_path_valid(socket_path))
		return (-1);
	if (!virtio_vhost_transport_registered(backend_id))
		return (-1);

	PRINTLN("connecting virtio vhost backend %s at %s", backend_id,
	    socket_path);
	fd = vhost_user_connect(socket_path);
	if (fd < 0) {
		EPRINTLN("failed to connect virtio vhost backend %s at %s: %s",
		    backend_id, socket_path, strerror(errno));
		return (-1);
	}
	if (virtio_vhost_transport_query_features(fd, &device_features) != 0) {
		EPRINTLN("failed to query virtio vhost backend %s features: %s",
		    backend_id, strerror(errno));
		close(fd);
		return (-1);
	}

	pthread_mutex_lock(&backends_lock);
	backend = virtio_vhost_transport_find_locked(backend_id);
	if (backend == NULL) {
		pthread_mutex_unlock(&backends_lock);
		close(fd);
		return (-1);
	}
	virtio_vhost_transport_close_fd(backend);
	virtio_vhost_transport_close_notify_fds(backend);
	backend->fd = fd;
	backend->connected = true;
	backend->setup_dirty = true;
	backend->device_features = device_features;
	backend->device_features_valid = true;
	backend->device_features_applied = backend->device_features_cb == NULL;
	device_features_cb = backend->device_features_cb;
	device_opaque = backend->device_opaque;
	pthread_cond_broadcast(&backends_cond);
	pthread_mutex_unlock(&backends_lock);

	if (device_features_cb != NULL) {
		device_features_cb(device_opaque, device_features);
		pthread_mutex_lock(&backends_lock);
		backend = virtio_vhost_transport_find_locked(backend_id);
		if (backend != NULL) {
			backend->device_features_applied = true;
			pthread_cond_broadcast(&backends_cond);
		}
		pthread_mutex_unlock(&backends_lock);
	}

	PRINTLN("virtio vhost backend %s connected: features=0x%llx",
	    backend_id, (unsigned long long)device_features);
	return (0);
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
	struct scorpi_virtio_vhost_transport_desc next_transport;
	int rc = 0;

	if (backend_id == NULL || transport == NULL)
		return (-1);
	if (transport->queue_count > SCORPI_VIRTIO_VHOST_MAX_QUEUES ||
	    transport->memory_region_count >
		SCORPI_VIRTIO_VHOST_MAX_MEMORY_REGIONS)
		return (-1);

	next_transport = *transport;
	snprintf(next_transport.backend_id, sizeof(next_transport.backend_id),
	    "%s", backend_id);

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
		if (virtio_vhost_transport_init_backend(backend) != 0) {
			free(backend);
			rc = -1;
			goto done;
		}
		LIST_INSERT_HEAD(&backends, backend, entries);
	}
	if (transport->device_name[0] != '\0')
		snprintf(backend->device_name, sizeof(backend->device_name),
		    "%s", transport->device_name);
	if (backend->device_name[0] != '\0')
		snprintf(next_transport.device_name,
		    sizeof(next_transport.device_name), "%s",
		    backend->device_name);
	if (memcmp(&backend->transport, &next_transport,
		sizeof(next_transport)) != 0) {
		backend->transport = next_transport;
		backend->setup_dirty = true;
	}
	pthread_cond_broadcast(&backends_cond);
done:
	pthread_mutex_unlock(&backends_lock);
	return (rc);
}

int
virtio_vhost_transport_bind_device(const char *backend_id,
    const struct scorpi_virtio_vhost_transport_desc *transport,
    virtio_vhost_interrupt_cb interrupt_cb,
    virtio_vhost_device_features_cb device_features_cb,
    virtio_vhost_event_cb event_cb, void *opaque)
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
	backend->device_features_cb = device_features_cb;
	backend->event_cb = event_cb;
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
	struct scorpi_virtio_vhost_transport_desc transport;
	pthread_mutex_t *io_mtx;
	bool setup_dirty;
	uint8_t value = VIRTIO_VHOST_NOTIFY_BYTE;
	int kick_fd;
	int fd;

	if (backend_id == NULL)
		return (-1);

	pthread_mutex_lock(&backends_lock);
	backend = virtio_vhost_transport_find_locked(backend_id);
	if (backend == NULL || !backend->connected || backend->fd < 0) {
		pthread_mutex_unlock(&backends_lock);
		return (-1);
	}
	fd = dup(backend->fd);
	if (fd < 0) {
		pthread_mutex_unlock(&backends_lock);
		return (-1);
	}
	io_mtx = &backend->io_mtx;
	transport = backend->transport;
	setup_dirty = backend->setup_dirty;
	pthread_mutex_unlock(&backends_lock);

	if (!virtio_vhost_transport_queue_ready(&transport, queue_index)) {
		close(fd);
		return (0);
	}

	pthread_mutex_lock(io_mtx);
	if (setup_dirty) {
		if (virtio_vhost_transport_send_transport_setup(backend, fd,
			&transport) != 0) {
			pthread_mutex_unlock(io_mtx);
			close(fd);
			return (-1);
		}
		pthread_mutex_lock(&backends_lock);
		if (memcmp(&backend->transport, &transport,
			sizeof(transport)) == 0)
			backend->setup_dirty = false;
		pthread_mutex_unlock(&backends_lock);
	}

	pthread_mutex_lock(&backends_lock);
	if (queue_index >= SCORPI_VIRTIO_VHOST_MAX_QUEUES)
		kick_fd = -1;
	else
		kick_fd = backend->kick_write_fds[queue_index];
	pthread_mutex_unlock(&backends_lock);
	if (kick_fd < 0 ||
	    (write(kick_fd, &value, sizeof(value)) < 0 && errno != EAGAIN &&
		errno != EWOULDBLOCK)) {
		pthread_mutex_unlock(io_mtx);
		close(fd);
		return (-1);
	}
	pthread_mutex_unlock(io_mtx);
	close(fd);
	return (0);
}

int
virtio_vhost_transport_notify_display_hint(const char *backend_id,
    uint32_t width, uint32_t height, bool hdpi)
{
	struct virtio_vhost_transport *backend;
	struct scorpi_vhost_msg msg;
	pthread_mutex_t *io_mtx;
	int fd;
	int rc;

	if (backend_id == NULL || width == 0 || height == 0)
		return (-1);

	pthread_mutex_lock(&backends_lock);
	backend = virtio_vhost_transport_find_locked(backend_id);
	if (backend == NULL || !backend->connected || backend->fd < 0) {
		pthread_mutex_unlock(&backends_lock);
		return (-1);
	}
	fd = dup(backend->fd);
	if (fd < 0) {
		pthread_mutex_unlock(&backends_lock);
		return (-1);
	}
	io_mtx = &backend->io_mtx;
	pthread_mutex_unlock(&backends_lock);

	vhost_user_msg_init(&msg, SCORPI_VHOST_MSG_DISPLAY_HINT,
	    sizeof(msg.payload.display_hint));
	msg.payload.display_hint.width = width;
	msg.payload.display_hint.height = height;
	msg.payload.display_hint.hdpi = hdpi ? 1 : 0;

	pthread_mutex_lock(io_mtx);
	rc = vhost_user_send_msg(fd, &msg, NULL, 0) < 0 ? -1 : 0;
	pthread_mutex_unlock(io_mtx);
	close(fd);
	return (rc);
}
