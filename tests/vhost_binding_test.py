#!/usr/bin/env python3
"""Test production binding reuse against live registered state, not a shadow cache."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/virtio_vhost_transport.c').read_text()
start = source.index('int\nvirtio_vhost_transport_bind_device(')
end = source.index('\nint\nvirtio_vhost_transport_notify_queue_kick(', start)
production = source[start:end]
prefix = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "virtio_vhost_transport.h"
struct virtio_vhost_transport {
    struct scorpi_virtio_vhost_transport_desc transport;
    virtio_vhost_interrupt_cb interrupt_cb;
    virtio_vhost_device_features_cb device_features_cb;
    virtio_vhost_event_cb event_cb;
    void *device_opaque;
    uint64_t device_features;
    bool device_features_valid, device_features_applied;
};
static struct virtio_vhost_transport backend;
static pthread_mutex_t backends_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t backends_cond = PTHREAD_COND_INITIALIZER;
static unsigned updates, callbacks;
static bool exists, fail_update;
static struct virtio_vhost_transport *virtio_vhost_transport_find_locked(const char *id) {
    return exists && id != NULL && strcmp(id, "test") == 0 ? &backend : NULL;
}
int virtio_vhost_transport_set_transport(const char *id,
    const struct scorpi_virtio_vhost_transport_desc *desc) {
    ++updates;
    if (id == NULL || desc == NULL || fail_update) return -1;
    pthread_mutex_lock(&backends_lock);
    backend.transport = *desc;
    exists = true;
    pthread_mutex_unlock(&backends_lock);
    return 0;
}
static void features(void *opaque, uint64_t value) {
    assert(opaque != NULL);
    assert(value == backend.device_features);
    ++callbacks;
}
static void interrupt(void *opaque, uint32_t index) { (void)opaque; (void)index; }
static void event(void *opaque, const struct scorpi_vhost_msg *msg) { (void)opaque; (void)msg; }
'''
suffix = r'''
int main(void) {
    struct scorpi_virtio_vhost_transport_desc desc = {0};
    int context, replacement;
    strcpy(desc.backend_id, "test");
    strcpy(desc.device_name, "virtio-gpu");
    desc.queue_count = 1;
    desc.queues[0].size = 1024;
    desc.queues[0].ready = true;
    desc.memory_region_count = 1;
    desc.memory_regions[0].size = 4096;
    strcpy(desc.memory_regions[0].shm_name, "/region");
    backend.device_features_valid = true;
    backend.device_features = 123;
#define BIND() virtio_vhost_transport_bind_device("test", &desc, interrupt, features, event, &context)
    assert(BIND() == 0);
    assert(updates == 1 && callbacks == 1);
    for (unsigned i = 0; i < 10000; ++i) assert(BIND() == 0);
    assert(updates == 1 && callbacks == 1);
    // Every live configuration input still causes an update.
#define CHANGE(field, value) do { unsigned n = updates; desc.field = value; \
    assert(BIND() == 0); assert(updates == n + 1); } while (0)
    CHANGE(features, 1);
    CHANGE(reset_generation, 1);
    CHANGE(queues[0].desc_addr, 0x1000);
    CHANGE(queues[0].avail_addr, 0x2000);
    CHANGE(queues[0].used_addr, 0x3000);
    CHANGE(queues[0].ready, false);
    CHANGE(queues[0].size, 512);
    CHANGE(queue_count, SCORPI_VIRTIO_VHOST_MAX_QUEUES);
    CHANGE(queues[SCORPI_VIRTIO_VHOST_MAX_QUEUES-1].desc_addr, 0x100000);
    CHANGE(queues[SCORPI_VIRTIO_VHOST_MAX_QUEUES-1].avail_addr, 0x104000);
    CHANGE(queues[SCORPI_VIRTIO_VHOST_MAX_QUEUES-1].used_addr, 0x105000);
    CHANGE(queues[SCORPI_VIRTIO_VHOST_MAX_QUEUES-1].ready, true);
    CHANGE(memory_regions[0].guest_phys_addr, 0x4000);
    CHANGE(memory_regions[0].size, 8192);
    CHANGE(memory_regions[0].shm_fd, 7);
    CHANGE(memory_regions[0].shm_offset, 4096);
    CHANGE(memory_regions[0].shm_name[1], 'x');
    CHANGE(memory_region_count, 0);
    CHANGE(queue_count, 0);
    CHANGE(ready, true);
    unsigned n = updates;
    // Clear registered transport, then republish an otherwise identical input.
    virtio_vhost_transport_clear_transport("test");
    assert(BIND() == 0 && updates == ++n);
    // Reconnect must finish any unapplied negotiated features.
    backend.device_features_applied = false;
    backend.device_features = 456;
    assert(BIND() == 0 && updates == ++n);
    assert(backend.device_features_applied);
    assert(virtio_vhost_transport_bind_device("test", &desc,
        interrupt, features, event, &replacement) == 0 && updates == ++n);
    assert(BIND() == 0 && updates == ++n);
    assert(virtio_vhost_transport_bind_device("test", &desc,
        NULL, NULL, NULL, &context) == 0 && updates == ++n);
    assert(BIND() == 0 && updates == ++n);
    fail_update = true;
    desc.reset_generation++;
    assert(BIND() == -1 && updates == ++n);
    fail_update = false;
    assert(BIND() == 0 && updates == ++n);
    assert(virtio_vhost_transport_bind_device(NULL, &desc, NULL, NULL, NULL, NULL) == -1);
    assert(virtio_vhost_transport_bind_device("test", NULL, NULL, NULL, NULL, NULL) == -1);
    puts("vhost binding: idempotence, callbacks, queue/memory/reset changes, reconnect and retry PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='scorpi-vhost-binding-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(prefix + production + suffix)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-I', str(root / 'include'),
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=20)
