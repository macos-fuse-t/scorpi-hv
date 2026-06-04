#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SCORPI_VIRTIO_VHOST_NAME_MAX 64
#define SCORPI_VIRTIO_VHOST_MAX_QUEUES 8
#define SCORPI_VIRTIO_VHOST_MAX_MEMORY_REGIONS 16
#define SCORPI_VIRTIO_VHOST_PROTOCOL "scorpi-vhost-v1"
#define SCORPI_VIRTIO_VHOST_DEVICE_GPU "virtio-gpu"
#define SCORPI_VIRTIO_VHOST_CMD_REGISTER "virtio_vhost_register"
#define SCORPI_VIRTIO_VHOST_CMD_DESCRIBE "virtio_vhost_describe"
#define SCORPI_VIRTIO_VHOST_CMD_QUEUE_KICK "virtio_vhost_queue_kick"
#define SCORPI_VIRTIO_VHOST_CMD_QUEUE_INTERRUPT \
	"virtio_vhost_queue_interrupt"
#define SCORPI_VIRTIO_VHOST_CMD_RESET "virtio_vhost_reset"
#define SCORPI_VIRTIO_VHOST_CMD_DISCONNECT "virtio_vhost_disconnect"
#define SCORPI_VIRTIO_VHOST_EVENT_QUEUE_KICK "virtio_vhost_queue_kick"
#define SCORPI_VIRTIO_VHOST_EVENT_GPU_RESIZE "virtio_vhost_gpu_resize"
#define SCORPI_VIRTIO_VHOST_CMD_GPU_CONFIG "virtio_vhost_gpu_config"

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
