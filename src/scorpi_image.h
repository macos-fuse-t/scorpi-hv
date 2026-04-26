/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#ifndef _SCORPI_IMAGE_H_
#define _SCORPI_IMAGE_H_

#include <sys/types.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <support/linker_set.h>

enum scorpi_image_format {
	SCORPI_IMAGE_FORMAT_RAW = 1,
	SCORPI_IMAGE_FORMAT_QCOW2,
	SCORPI_IMAGE_FORMAT_SCO,
};

enum scorpi_image_extent_state {
	SCORPI_IMAGE_EXTENT_ABSENT = 0,
	SCORPI_IMAGE_EXTENT_PRESENT,
	SCORPI_IMAGE_EXTENT_ZERO,
	SCORPI_IMAGE_EXTENT_DISCARDED,
};

struct scorpi_image_info {
	enum scorpi_image_format format;
	uint64_t virtual_size;
	uint32_t logical_sector_size;
	uint32_t physical_sector_size;
	uint32_t cluster_size;
	bool readonly;
	bool sealed;
	bool can_discard;
	uint8_t image_uuid[16];
	bool has_image_uuid;
	uint8_t image_digest[32];
	bool has_image_digest;
	bool has_base;
	char *base_uri;
	uint8_t base_uuid[16];
	bool has_base_uuid;
	uint8_t base_digest[32];
	bool has_base_digest;
};

struct scorpi_image_extent {
	uint64_t offset;
	uint64_t length;
	enum scorpi_image_extent_state state;
};

struct scorpi_image_ops {
	const char *name;
	int (*probe)(int fd, uint32_t *score);
	int (*open)(const char *path, int fd, bool readonly, void **state);
	int (*get_info)(void *state, struct scorpi_image_info *info);
	int (*map)(void *state, uint64_t offset, uint64_t length,
	    struct scorpi_image_extent *extent);
	int (*read)(void *state, void *buf, uint64_t offset, size_t len);
	int (*write)(void *state, const void *buf, uint64_t offset,
	    size_t len);
	int (*discard)(void *state, uint64_t offset, uint64_t length);
	int (*flush)(void *state);
	int (*close)(void *state);
};

struct scorpi_image {
	const struct scorpi_image_ops *ops;
	void *state;
	struct scorpi_image_info info;
	char *path;
	char *source_uri;
	dev_t dev;
	ino_t ino;
	bool has_file_id;
	struct scorpi_image *base;
};

#define	SCORPI_IMAGE_BACKEND_SET(x)	DATA_SET(sco_img_be_set, x)

const struct scorpi_image_ops *scorpi_image_backend_find(const char *name);
int	scorpi_image_backend_probe(int fd, const struct scorpi_image_ops **ops,
	    uint32_t *score);

#endif /* _SCORPI_IMAGE_H_ */
