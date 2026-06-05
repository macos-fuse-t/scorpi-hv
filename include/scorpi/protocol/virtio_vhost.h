#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SCORPI_VIRTIO_VHOST_NAME_MAX 64
#define SCORPI_VIRTIO_VHOST_MAX_QUEUES 8
#define SCORPI_VIRTIO_VHOST_MAX_MEMORY_REGIONS 16
#define SCORPI_VIRTIO_VHOST_PROTOCOL "scorpi-vhost-v1"
#define SCORPI_VIRTIO_VHOST_DEVICE_GPU "virtio-gpu"

struct scorpi_virtio_vhost_queue_desc {
	uint32_t index;
	uint32_t size;
	uint64_t desc_addr;
	uint64_t avail_addr;
	uint64_t used_addr;
	bool ready;
};

struct scorpi_virtio_vhost_memory_region_desc {
	uint32_t index;
	uint64_t guest_phys_addr;
	uint64_t size;
	char shm_name[SCORPI_VIRTIO_VHOST_NAME_MAX];
	uint64_t shm_offset;
};

struct scorpi_virtio_vhost_transport_desc {
	char backend_id[SCORPI_VIRTIO_VHOST_NAME_MAX];
	char device_name[SCORPI_VIRTIO_VHOST_NAME_MAX];
	uint64_t features;
	uint32_t reset_generation;
	uint32_t queue_count;
	struct scorpi_virtio_vhost_queue_desc
	    queues[SCORPI_VIRTIO_VHOST_MAX_QUEUES];
	uint32_t memory_region_count;
	struct scorpi_virtio_vhost_memory_region_desc
	    memory_regions[SCORPI_VIRTIO_VHOST_MAX_MEMORY_REGIONS];
	bool ready;
};
