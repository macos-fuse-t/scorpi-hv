#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <scorpi/protocol/virtio_external.h>

typedef void (*virtio_external_interrupt_cb)(void *opaque,
    uint32_t queue_index);
typedef void (*virtio_external_reset_cb)(void *opaque);

void virtio_external_backend_init(void);
bool virtio_external_backend_registered(const char *backend_id);
void virtio_external_backend_wait_bound_connected(void);
int virtio_external_backend_set_transport(const char *backend_id,
    const struct scorpi_virtio_external_transport_desc *transport);
int virtio_external_backend_bind_device(const char *backend_id,
    const struct scorpi_virtio_external_transport_desc *transport,
    virtio_external_interrupt_cb interrupt_cb,
    virtio_external_reset_cb reset_cb, void *opaque);
void virtio_external_backend_clear_transport(const char *backend_id);
int virtio_external_backend_notify_queue_kick(const char *backend_id,
    uint32_t queue_index);
int virtio_external_backend_notify_display_resize(const char *backend_id,
    uint32_t width, uint32_t height);
