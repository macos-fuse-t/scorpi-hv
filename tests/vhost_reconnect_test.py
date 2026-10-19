#!/usr/bin/env python3
"""Exercise socket replacement during event receive using production functions."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/virtio_vhost_transport.c').read_text()

def section(start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]

production = section('static int\nvirtio_vhost_transport_queue_event(', '\nstatic int\nvirtio_vhost_transport_send_request_fds(')
production += section('static void\nvirtio_vhost_transport_handle_backend_event(',
                     '\nstatic int\nvirtio_vhost_transport_send_memory_table(')
production += section('static void\nvirtio_vhost_transport_control_event(',
                      '\nint\nvirtio_vhost_transport_connect_configured_backends(')

prefix = r'''
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include "virtio_vhost_transport.h"
#define VHOST_USER_MAX_FDS 8
#define PRINTLN(...) ((void)0)
#define EPRINTLN(...) ((void)0)
enum ev_type { EVF_READ };
struct mevent { int fd; };
struct virtio_vhost_pending_event {
    struct scorpi_vhost_msg msg;
    struct virtio_vhost_pending_event *next;
};
struct virtio_vhost_transport {
    int fd;
    struct mevent *control_event;
    pthread_mutex_t connect_mtx;
    pthread_mutex_t io_mtx;
    struct virtio_vhost_pending_event *pending_events_head, *pending_events_tail;
    bool dispatching_events;
    bool connected, setup_dirty, device_features_valid, device_features_applied;
    uint64_t device_features;
    struct scorpi_vhost_queue_reset queue_resets[SCORPI_VIRTIO_VHOST_MAX_QUEUES];
    struct scorpi_virtio_vhost_transport_desc transport;
    virtio_vhost_event_cb event_cb;
    virtio_vhost_device_features_cb device_features_cb;
    void *device_opaque;
};
static struct virtio_vhost_transport backend;
static pthread_mutex_t backends_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t backends_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static bool reached_io, fail_event;
static _Thread_local bool gate_receiver;
static int next_fd;
static unsigned reads, events, closed_queues;
static struct mevent control_event;
static int test_lock(pthread_mutex_t *mutex) {
    if (mutex == &backend.io_mtx && gate_receiver) {
        gate_receiver = false;
        pthread_mutex_lock(&gate_lock);
        reached_io = true;
        pthread_cond_signal(&gate_cond);
        pthread_mutex_unlock(&gate_lock);
    }
    return pthread_mutex_lock(mutex);
}
static bool virtio_vhost_transport_fd_readable(int fd) {
    // The old descriptor has been reused for an unrelated socket.
    assert(fd == backend.fd);
    struct pollfd p = {.fd=fd, .events=POLLIN};
    return poll(&p, 1, 0) == 1 && (p.revents & POLLIN);
}
static ssize_t vhost_user_recv_msg(int fd, struct scorpi_vhost_msg *msg,
    int *fds, size_t capacity, size_t *count) {
    (void)fds; (void)capacity; *count = 0; ++reads;
    assert(fd == backend.fd);
    assert(pthread_mutex_trylock(&backend.io_mtx) == EBUSY);
    return recv(fd, msg, sizeof(*msg), 0);
}
static void virtio_vhost_transport_close_recv_fds(int *fds, size_t count) {
    for (size_t i=0; i<count; ++i) if (fds[i]>=0) close(fds[i]);
}
static void virtio_vhost_transport_close_queue_notify_fds(
    struct virtio_vhost_transport *b, size_t index) {
    assert(b == &backend && index == 4); ++closed_queues;
}
static void virtio_vhost_transport_dispatch_events(struct virtio_vhost_transport *);
static void on_event(void *opaque, const struct scorpi_vhost_msg *msg) {
    assert(opaque == &backend && msg->header.request == SCORPI_VHOST_MSG_QUEUE_RESET_DONE);
    // MMIO may hold the device mutex and need io_mtx: callbacks must not hold I/O.
    assert(pthread_mutex_trylock(&backend.io_mtx) == 0);
    pthread_mutex_unlock(&backend.io_mtx);
    ++events;
    // A device callback may re-enter a transport operation that drains events.
    // The active dispatcher must retain ownership rather than recurse.
    virtio_vhost_transport_dispatch_events(&backend);
}
static void on_features(void *opaque, uint64_t features) {
    assert(opaque == &backend && features == (1ULL << 40));
    assert(pthread_mutex_trylock(&backend.io_mtx) == 0);
    pthread_mutex_unlock(&backend.io_mtx);
    assert(pthread_mutex_trylock(&backend.connect_mtx) == EBUSY);
}
static bool virtio_vhost_transport_valid_name(const char *name) { return name && *name; }
static bool virtio_vhost_transport_socket_path_valid(const char *path) { return path && *path; }
bool virtio_vhost_transport_registered(const char *id) { return !strcmp(id, "test"); }
static struct virtio_vhost_transport *virtio_vhost_transport_find_locked(const char *id) {
    return !strcmp(id,"test") ? &backend : NULL;
}
static int vhost_user_connect(const char *path) { (void)path; return next_fd; }
static int virtio_vhost_transport_query_features(int fd, uint64_t *features) {
    assert(fd == next_fd); *features = 1ULL << 40; return 0;
}
static void virtio_vhost_transport_close_fd(struct virtio_vhost_transport *b) {
    assert(pthread_mutex_trylock(&b->io_mtx) == EBUSY);
    if(b->fd >= 0) close(b->fd);
    b->fd=-1; b->control_event=NULL;
}
static void virtio_vhost_transport_close_notify_fds(struct virtio_vhost_transport *b) {
    assert(b == &backend);
}
static struct mevent *mevent_add(int fd, enum ev_type type,
    void (*callback)(int, enum ev_type, void *), void *opaque) {
    (void)type; (void)callback; assert(opaque == &backend);
    control_event.fd = fd; return fail_event ? NULL : &control_event;
}
static void virtio_vhost_transport_handle_backend_event(
    struct virtio_vhost_transport *, const struct scorpi_vhost_msg *);
#define pthread_mutex_lock test_lock
'''

suffix = r'''
#undef pthread_mutex_lock
static void *receive_thread(void *unused) {
    (void)unused; gate_receiver = true;
    virtio_vhost_transport_recv_pending_events(&backend); return NULL;
}
int main(void) {
    int old[2], current[2], unrelated[2];
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, old) == 0);
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, current) == 0);
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, unrelated) == 0);
    assert(pthread_mutex_init(&backend.io_mtx, NULL) == 0);
    assert(pthread_mutex_init(&backend.connect_mtx, NULL) == 0);
    backend.fd=old[0]; backend.connected=true;
    backend.event_cb=on_event; backend.device_opaque=&backend;
    backend.transport.reset_generation=7;
    struct scorpi_vhost_msg msg={0};
    msg.header.request=SCORPI_VHOST_MSG_QUEUE_RESET_DONE;
    msg.header.size=sizeof(msg.payload.queue_reset);
    msg.payload.queue_reset=(struct scorpi_vhost_queue_reset){.index=4,.device_generation=7,.serial=42};
    backend.queue_resets[4]=msg.payload.queue_reset;
    assert(send(current[1], &msg, sizeof(msg), 0) == sizeof(msg));
    assert(send(unrelated[1], &msg, sizeof(msg), 0) == sizeof(msg));

    // Force replacement after the receiver started, but before it takes I/O.
    pthread_mutex_lock(&backend.io_mtx);
    pthread_t receiver;
    assert(pthread_create(&receiver, NULL, receive_thread, NULL) == 0);
    pthread_mutex_lock(&gate_lock);
    while(!reached_io) pthread_cond_wait(&gate_cond, &gate_lock);
    pthread_mutex_unlock(&gate_lock);
    pthread_mutex_lock(&backends_lock);
    assert(dup2(unrelated[0], old[0]) == old[0]);
    backend.fd=current[0];
    pthread_mutex_unlock(&backends_lock);
    pthread_mutex_unlock(&backend.io_mtx);
    assert(pthread_join(receiver,NULL) == 0);
    assert(reads == 1 && events == 1 && closed_queues == 1);
    assert(backend.queue_resets[4].serial == 0 && backend.setup_dirty);
    // Duplicate acknowledgements cannot release the slot twice.
    assert(send(current[1], &msg, sizeof(msg), 0) == sizeof(msg));
    virtio_vhost_transport_recv_pending_events(&backend);
    assert(events == 1 && closed_queues == 1);

    // Unsolicited events encountered while waiting for a synchronous reply
    // are retained, then dispatched after the I/O transaction has unlocked.
    ++msg.payload.queue_reset.serial;
    backend.queue_resets[4]=msg.payload.queue_reset;
    struct scorpi_vhost_msg reply={0}, received;
    reply.header.request=SCORPI_VHOST_MSG_SET_FEATURES;
    reply.header.flags=SCORPI_VHOST_FLAG_REPLY;
    assert(send(current[1], &msg, sizeof(msg), 0) == sizeof(msg));
    assert(send(current[1], &reply, sizeof(reply), 0) == sizeof(reply));
    pthread_mutex_lock(&backend.io_mtx);
    assert(virtio_vhost_transport_recv_reply(&backend,backend.fd,reply.header.request,&received)==0);
    assert(events==1 && backend.pending_events_head);
    pthread_mutex_unlock(&backend.io_mtx);
    virtio_vhost_transport_dispatch_events(&backend);
    assert(events==2 && !backend.pending_events_head && !backend.dispatching_events);

    int replacement[2]; assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, replacement) == 0);
    backend.device_features_cb=on_features;
    next_fd=replacement[0]; backend.queue_resets[4]=msg.payload.queue_reset;
    assert(virtio_vhost_transport_connect_backend("test","test.sock") == 0);
    assert(backend.connected && backend.fd==replacement[0] && backend.control_event);
    assert(backend.device_features_applied);
    assert(backend.queue_resets[4].serial == 0);
    assert(send(replacement[1], &msg, sizeof(msg), 0) == sizeof(msg));
    virtio_vhost_transport_recv_pending_events(&backend);
    assert(events == 2); // The previous connection's request is no longer live.

    int failed[2]; assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, failed) == 0);
    next_fd=failed[0]; fail_event=true;
    assert(virtio_vhost_transport_connect_backend("test","test.sock") == -1);
    assert(!backend.connected && backend.fd==-1);
    virtio_vhost_transport_recv_pending_events(&backend);
    close(old[0]); close(old[1]); close(current[1]);
    close(unrelated[0]); close(unrelated[1]); close(replacement[1]); close(failed[1]);
    pthread_mutex_destroy(&backend.io_mtx);
    pthread_mutex_destroy(&backend.connect_mtx);
    puts("PASS: reconnect/receive FD reuse, deferred ACK dispatch outside I/O, stale ACK rejection and connect failure");
}
'''

with tempfile.TemporaryDirectory(prefix='scorpi-vhost-reconnect-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(prefix + production + suffix)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-I', str(root / 'include'),
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=20)
