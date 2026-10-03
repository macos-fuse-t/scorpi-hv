#!/usr/bin/env python3
"""Exercise the production notifier with real nonblocking pipes and setup stubs."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/virtio_vhost_transport.c').read_text()
start = source.index('int\nvirtio_vhost_transport_notify_queue_kick(')
end = source.index('\nint\nvirtio_vhost_transport_notify_display_hint(', start)
production = source[start:end]
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#define SCORPI_VIRTIO_VHOST_MAX_QUEUES 4
#define VIRTIO_VHOST_NOTIFY_BYTE 1
struct scorpi_virtio_vhost_transport_desc { bool ready[4]; };
struct virtio_vhost_transport {
    bool connected, setup_dirty;
    int fd, kick_write_fds[4];
    pthread_mutex_t io_mtx;
    struct scorpi_virtio_vhost_transport_desc transport;
};
static pthread_mutex_t backends_lock = PTHREAD_MUTEX_INITIALIZER;
static struct virtio_vhost_transport backend;
static unsigned duplicates, setups;
static bool interrupt_write, fail_setup, check_lifetime_lock;
static int test_dup(int fd) { ++duplicates; return dup(fd); }
static ssize_t test_write(int fd, const void *bytes, size_t size) {
    if (check_lifetime_lock)
        assert(pthread_mutex_trylock(&backends_lock) == EBUSY);
    if (interrupt_write) { interrupt_write = false; errno = EINTR; return -1; }
    return write(fd, bytes, size);
}
static struct virtio_vhost_transport *virtio_vhost_transport_find_locked(const char *id) {
    return strcmp(id, "test") == 0 ? &backend : NULL;
}
static bool virtio_vhost_transport_queue_ready(
    const struct scorpi_virtio_vhost_transport_desc *state, uint32_t index) {
    return index < 4 && state->ready[index];
}
static int virtio_vhost_transport_send_transport_setup(
    struct virtio_vhost_transport *state, int fd,
    const struct scorpi_virtio_vhost_transport_desc *desc) {
    (void)state; (void)fd; (void)desc; ++setups; return fail_setup ? -1 : 0;
}
#define dup test_dup
#define write test_write
'''
suffix = r'''
#undef dup
#undef write
static void *kick_thread(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 10000; ++i)
        assert(virtio_vhost_transport_notify_queue_kick("test", 0) == 0);
    return NULL;
}
int main(void) {
    int descriptors[2];
    assert(pipe(descriptors) == 0);
    assert(fcntl(descriptors[0], F_SETFL, O_NONBLOCK) == 0);
    assert(fcntl(descriptors[1], F_SETFL, O_NONBLOCK) == 0);
    backend.connected = true;
    backend.fd = descriptors[0];
    for (unsigned i = 0; i < 4; ++i) backend.kick_write_fds[i] = -1;
    backend.kick_write_fds[0] = descriptors[1];
    backend.transport.ready[0] = true;
    assert(pthread_mutex_init(&backend.io_mtx, NULL) == 0);
    // A clean kick must not take the control mutex or duplicate its socket.
    pthread_mutex_lock(&backend.io_mtx);
    check_lifetime_lock = true;
    interrupt_write = true;
    assert(virtio_vhost_transport_notify_queue_kick("test", 0) == 0);
    pthread_mutex_unlock(&backend.io_mtx);
    unsigned char byte;
    assert(read(descriptors[0], &byte, 1) == 1 && byte == 1);
    assert(duplicates == 0 && setups == 0 && !interrupt_write);
    assert(virtio_vhost_transport_notify_queue_kick(NULL, 0) == -1);
    assert(virtio_vhost_transport_notify_queue_kick("unknown", 0) == -1);
    assert(virtio_vhost_transport_notify_queue_kick("test", 4) == -1);
    assert(virtio_vhost_transport_notify_queue_kick("test", 1) == 0);
    backend.transport.ready[1] = true;
    assert(virtio_vhost_transport_notify_queue_kick("test", 1) == -1);
    check_lifetime_lock = false;
    backend.setup_dirty = true;
    fail_setup = true;
    assert(virtio_vhost_transport_notify_queue_kick("test", 0) == -1);
    assert(backend.setup_dirty && duplicates == 1 && setups == 1);
    fail_setup = false;
    assert(virtio_vhost_transport_notify_queue_kick("test", 0) == 0);
    assert(!backend.setup_dirty && duplicates == 2 && setups == 2);
    check_lifetime_lock = true;
    // Burst notifications must accept a full pipe as an existing wakeup.
    // No descriptor can be closed/replaced while a writer holds lifetime lock.
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; ++i) assert(pthread_create(&threads[i], NULL, kick_thread, NULL) == 0);
    for (unsigned i = 0; i < 2; ++i) assert(pthread_join(threads[i], NULL) == 0);
    assert(duplicates == 2 && setups == 2);
    backend.connected = false;
    assert(virtio_vhost_transport_notify_queue_kick("test", 0) == -1);
    close(descriptors[0]); close(descriptors[1]);
    pthread_mutex_destroy(&backend.io_mtx);
    puts("vhost kick: direct notifier, setup, invalid/closed descriptors, EINTR, full pipe and concurrent bursts PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='scorpi-vhost-kick-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(prefix + production + suffix)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=20)
