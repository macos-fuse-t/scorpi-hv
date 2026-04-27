/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/errno.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "scorpi_image_raw.h"

struct scorpi_raw_image {
	int fd;
	bool readonly;
	uint64_t size;
};

static int
raw_probe(int fd __attribute__((unused)), uint32_t *score)
{
	*score = 1;
	return (0);
}

static int
raw_open(const char *path __attribute__((unused)), int fd, bool readonly,
    void **state)
{
	struct scorpi_raw_image *raw;
	struct stat sb;

	if (state == NULL || fd < 0)
		return (EINVAL);
	if (fstat(fd, &sb) != 0)
		return (errno);

	raw = calloc(1, sizeof(*raw));
	if (raw == NULL)
		return (ENOMEM);

	raw->fd = fd;
	raw->readonly = readonly;
	raw->size = (uint64_t)sb.st_size;
	*state = raw;
	return (0);
}

static int
raw_get_info(void *state, struct scorpi_image_info *info)
{
	struct scorpi_raw_image *raw;

	if (state == NULL || info == NULL)
		return (EINVAL);

	raw = state;
	*info = (struct scorpi_image_info){
		.format = SCORPI_IMAGE_FORMAT_RAW,
		.virtual_size = raw->size,
		.logical_sector_size = 512,
		.physical_sector_size = 0,
		.cluster_size = 0,
		.readonly = raw->readonly,
		.sealed = raw->readonly,
		.has_base = false,
	};
	return (0);
}

static int
raw_map(void *state, uint64_t offset, uint64_t length,
    struct scorpi_image_extent *extent)
{
	struct scorpi_raw_image *raw;

	if (state == NULL || extent == NULL)
		return (EINVAL);

	raw = state;
	if (offset > raw->size)
		return (EINVAL);
	if (length > raw->size - offset)
		length = raw->size - offset;

	*extent = (struct scorpi_image_extent){
		.offset = offset,
		.length = length,
		.state = SCORPI_IMAGE_EXTENT_PRESENT,
	};
	return (0);
}

static int
raw_read(void *state, void *buf, uint64_t offset, size_t len)
{
	struct scorpi_raw_image *raw;
	ssize_t n;

	if (state == NULL || buf == NULL)
		return (EINVAL);

	raw = state;
	n = pread(raw->fd, buf, len, (off_t)offset);
	if (n < 0)
		return (errno);
	if ((size_t)n != len)
		return (EIO);
	return (0);
}

static int
raw_write(void *state, const void *buf, uint64_t offset, size_t len)
{
	struct scorpi_raw_image *raw;
	ssize_t n;

	if (state == NULL || buf == NULL)
		return (EINVAL);

	raw = state;
	if (raw->readonly)
		return (EROFS);

	n = pwrite(raw->fd, buf, len, (off_t)offset);
	if (n < 0)
		return (errno);
	if ((size_t)n != len)
		return (EIO);
	return (0);
}

static int
raw_discard(void *state)
{
	if (state == NULL)
		return (EINVAL);
	return (EOPNOTSUPP);
}

static int
raw_flush(void *state)
{
	struct scorpi_raw_image *raw;

	if (state == NULL)
		return (EINVAL);

	raw = state;
	if (fsync(raw->fd) != 0)
		return (errno);
	return (0);
}

static int
raw_close(void *state)
{
	struct scorpi_raw_image *raw;
	int error;

	if (state == NULL)
		return (0);

	raw = state;
	error = 0;
	if (raw->fd >= 0 && close(raw->fd) != 0)
		error = errno;
	free(raw);
	return (error);
}

static int
raw_discard_range(void *state, uint64_t offset __attribute__((unused)),
    uint64_t length __attribute__((unused)))
{
	return (raw_discard(state));
}

static const struct scorpi_image_ops scorpi_raw_ops = {
	.name = "raw",
	.probe = raw_probe,
	.open = raw_open,
	.get_info = raw_get_info,
	.map = raw_map,
	.read = raw_read,
	.write = raw_write,
	.discard = raw_discard_range,
	.flush = raw_flush,
	.close = raw_close,
};

const struct scorpi_image_ops *
scorpi_image_raw_backend(void)
{
	return (&scorpi_raw_ops);
}
