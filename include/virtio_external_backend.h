#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIRTIO_EXTERNAL_NAME_MAX 64
#define VIRTIO_EXTERNAL_MAX_QUEUES 8
#define VIRTIO_EXTERNAL_MAX_MEMORY_REGIONS 16

struct virtio_external_queue_desc {
	uint32_t index;
	uint32_t size;
	uint64_t desc_addr;
	uint64_t avail_addr;
	uint64_t used_addr;
	bool ready;
};

struct virtio_external_memory_region_desc {
	uint32_t index;
	uint64_t guest_phys_addr;
	uint64_t size;
	char shm_name[VIRTIO_EXTERNAL_NAME_MAX];
	uint64_t shm_offset;
};

struct virtio_external_transport_desc {
	char backend_id[VIRTIO_EXTERNAL_NAME_MAX];
	char device_name[VIRTIO_EXTERNAL_NAME_MAX];
	uint64_t features;
	uint32_t reset_generation;
	uint32_t queue_count;
	struct virtio_external_queue_desc queues[VIRTIO_EXTERNAL_MAX_QUEUES];
	uint32_t memory_region_count;
	struct virtio_external_memory_region_desc
	    memory_regions[VIRTIO_EXTERNAL_MAX_MEMORY_REGIONS];
	bool ready;
};

typedef void (*virtio_external_interrupt_cb)(void *opaque,
    uint32_t queue_index);
typedef void (*virtio_external_reset_cb)(void *opaque);

void virtio_external_backend_init(void);
bool virtio_external_backend_registered(const char *backend_id);
int virtio_external_backend_set_transport(const char *backend_id,
    const struct virtio_external_transport_desc *transport);
int virtio_external_backend_bind_device(const char *backend_id,
    const struct virtio_external_transport_desc *transport,
    virtio_external_interrupt_cb interrupt_cb,
    virtio_external_reset_cb reset_cb, void *opaque);
void virtio_external_backend_clear_transport(const char *backend_id);
int virtio_external_backend_notify_queue_kick(const char *backend_id,
    uint32_t queue_index);
