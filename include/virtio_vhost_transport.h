#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <scorpi/protocol/virtio_vhost.h>
#include <scorpi/protocol/vhost_user.h>

typedef void (*virtio_vhost_interrupt_cb)(void *opaque,
    uint32_t queue_index);
typedef void (*virtio_vhost_reset_cb)(void *opaque);
typedef void (*virtio_vhost_device_features_cb)(void *opaque,
    uint64_t device_features);
typedef void (*virtio_vhost_event_cb)(void *opaque,
    const struct scorpi_vhost_msg *msg);

void virtio_vhost_transport_init(void);
bool virtio_vhost_transport_registered(const char *backend_id);
int virtio_vhost_transport_set_backend_socket(const char *backend_id,
    const char *socket_path);
int virtio_vhost_transport_connect_configured_backends(void);
int virtio_vhost_transport_wait_configured_backends_registered(void);
int virtio_vhost_transport_connect_backend(const char *backend_id,
    const char *socket_path);
int virtio_vhost_transport_set_transport(const char *backend_id,
    const struct scorpi_virtio_vhost_transport_desc *transport);
int virtio_vhost_transport_bind_device(const char *backend_id,
    const struct scorpi_virtio_vhost_transport_desc *transport,
    virtio_vhost_interrupt_cb interrupt_cb,
    virtio_vhost_reset_cb reset_cb,
    virtio_vhost_device_features_cb device_features_cb,
    virtio_vhost_event_cb event_cb, void *opaque);
void virtio_vhost_transport_clear_transport(const char *backend_id);
int virtio_vhost_transport_notify_queue_kick(const char *backend_id,
    uint32_t queue_index);
int virtio_vhost_transport_notify_display_resize(const char *backend_id,
    uint32_t width, uint32_t height);
