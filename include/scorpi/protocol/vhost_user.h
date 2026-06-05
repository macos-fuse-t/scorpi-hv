#pragma once

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Scorpi vhost transport.
 *
 * This is not QEMU vhost-user wire compatible. It keeps the same architecture:
 * a frontend exports memory/vrings and a backend owns virtqueue processing.
 */

#define SCORPI_VHOST_MAGIC 0x56534350u /* "PCSV" little endian. */
#define SCORPI_VHOST_VERSION 1u
#define SCORPI_VHOST_FLAG_REPLY 0x1u
#define SCORPI_VHOST_FLAG_NEED_REPLY 0x2u
#define SCORPI_VHOST_MAX_MEMORY_REGIONS 16
#define SCORPI_VHOST_MAX_QUEUES 16
#define SCORPI_VHOST_MAX_CONFIG_SIZE 256
#define SCORPI_VHOST_SCANOUT_NAME_MAX 31

enum scorpi_vhost_msg_type {
	SCORPI_VHOST_MSG_NONE = 0,
	SCORPI_VHOST_MSG_HELLO = 1,
	SCORPI_VHOST_MSG_FEATURES = 2,
	SCORPI_VHOST_MSG_SET_FEATURES = 3,
	SCORPI_VHOST_MSG_SET_MEM_TABLE = 4,
	SCORPI_VHOST_MSG_SET_QUEUE_NUM = 5,
	SCORPI_VHOST_MSG_SET_QUEUE_ADDR = 6,
	SCORPI_VHOST_MSG_SET_QUEUE_BASE = 7,
	SCORPI_VHOST_MSG_GET_QUEUE_BASE = 8,
	SCORPI_VHOST_MSG_SET_QUEUE_KICK = 9,
	SCORPI_VHOST_MSG_SET_QUEUE_CALL = 10,
	SCORPI_VHOST_MSG_SET_QUEUE_ENABLE = 11,
	SCORPI_VHOST_MSG_GET_CONFIG = 12,
	SCORPI_VHOST_MSG_SET_CONFIG = 13,
	SCORPI_VHOST_MSG_SET_STATUS = 14,
	SCORPI_VHOST_MSG_RESET_DEVICE = 15,
	SCORPI_VHOST_MSG_QUEUE_KICK = 16,
	SCORPI_VHOST_MSG_QUEUE_CALL = 17,
	SCORPI_VHOST_MSG_ERROR = 18,
	SCORPI_VHOST_MSG_SCANOUT = 19,
	SCORPI_VHOST_MSG_DIRTY_RECT = 20,
	SCORPI_VHOST_MSG_DISPLAY_HINT = 21,
};

struct scorpi_vhost_header {
	uint32_t magic;
	uint16_t version;
	uint16_t header_size;
	uint32_t request;
	uint32_t flags;
	uint32_t size;
};

struct scorpi_vhost_hello {
	uint64_t device_id;
	uint64_t frontend_features;
	uint64_t backend_features;
	uint32_t queue_count;
	uint32_t config_size;
};

struct scorpi_vhost_queue_state {
	uint32_t index;
	uint32_t num;
};

struct scorpi_vhost_queue_addr {
	uint32_t index;
	uint32_t flags;
	uint64_t desc_user_addr;
	uint64_t used_user_addr;
	uint64_t avail_user_addr;
	uint64_t log_guest_addr;
};

struct scorpi_vhost_memory_region {
	uint64_t guest_phys_addr;
	uint64_t memory_size;
	uint64_t userspace_addr;
	uint64_t mmap_offset;
};

struct scorpi_vhost_memory {
	uint32_t nregions;
	uint32_t padding;
	struct scorpi_vhost_memory_region regions[SCORPI_VHOST_MAX_MEMORY_REGIONS];
};

struct scorpi_vhost_config {
	uint32_t offset;
	uint32_t size;
	uint32_t flags;
	uint8_t region[SCORPI_VHOST_MAX_CONFIG_SIZE];
};

#define SCORPI_VHOST_SCANOUT_F_ENABLED 0x1u

struct scorpi_vhost_scanout {
	uint32_t flags;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
	uint32_t backing_width;
	uint32_t backing_height;
	uint32_t reserved;
	uint64_t shm_size;
	char shm_name[SCORPI_VHOST_SCANOUT_NAME_MAX + 1];
};

struct scorpi_vhost_dirty_rect {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

struct scorpi_vhost_display_hint {
	uint32_t width;
	uint32_t height;
	uint32_t hdpi;
	uint32_t reserved;
};

struct scorpi_vhost_msg {
	struct scorpi_vhost_header header;
	union {
		uint64_t u64;
		struct scorpi_vhost_hello hello;
		struct scorpi_vhost_queue_state queue_state;
		struct scorpi_vhost_queue_addr queue_addr;
		struct scorpi_vhost_memory memory;
		struct scorpi_vhost_config config;
		struct scorpi_vhost_scanout scanout;
		struct scorpi_vhost_dirty_rect dirty_rect;
		struct scorpi_vhost_display_hint display_hint;
	} payload;
};

#define SCORPI_VHOST_HEADER_SIZE \
	((uint32_t)offsetof(struct scorpi_vhost_msg, payload))

static inline int
scorpi_vhost_scanout_name_from_socket(const char *socket_path, char *out,
    size_t out_len)
{
	const char *base;
	size_t base_len;
	size_t pos;
	size_t written = 0;

	if (socket_path == NULL || out == NULL || out_len == 0)
		return (-1);

	base = strrchr(socket_path, '/');
	base = base == NULL ? socket_path : base + 1;
	base_len = strlen(base);
	if (base_len == 0)
		return (-1);
	if (base_len > 5 && strcmp(base + base_len - 5, ".sock") == 0)
		base_len -= 5;

	if (out_len > SCORPI_VHOST_SCANOUT_NAME_MAX)
		out_len = SCORPI_VHOST_SCANOUT_NAME_MAX;
	if (out_len < sizeof("/sr-a-0"))
		return (-1);

	memcpy(out, "/sr-", sizeof("/sr-") - 1);
	pos = sizeof("/sr-") - 1;
	for (size_t i = 0; i < base_len && pos + sizeof("-0") < out_len; i++) {
		unsigned char ch = (unsigned char)base[i];

		if (isalnum(ch) || ch == '-' || ch == '_') {
			out[pos++] = (char)ch;
			written++;
		} else {
			out[pos++] = '_';
			written++;
		}
	}
	if (written == 0)
		return (-1);
	out[pos++] = '-';
	out[pos++] = '0';
	out[pos] = '\0';
	return (0);
}
