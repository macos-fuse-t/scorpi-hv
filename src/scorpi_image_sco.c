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
#include <string.h>
#include <unistd.h>

#include <support/endian.h>

#include "scorpi_image_sco.h"

#define	SCO_MAGIC			"SCOIMG\0\0"
#define	SCO_MAGIC_SIZE			8
#define	SCO_FORMAT_MAJOR		1
#define	SCO_FILE_ID_OFFSET		0x00000000ULL
#define	SCO_FILE_ID_SIZE		0x00001000U
#define	SCO_SUPERBLOCK_A_OFFSET		0x00001000ULL
#define	SCO_SUPERBLOCK_B_OFFSET		0x00002000ULL
#define	SCO_SUPERBLOCK_SIZE		0x00001000U
#define	SCO_METADATA_AREA_OFFSET		0x00010000ULL
#define	SCO_METADATA_PAGE_SIZE		0x00001000U
#define	SCO_MIN_FILE_SIZE		0x00010000ULL
#define	SCO_MIN_CLUSTER_SIZE		0x00010000U
#define	SCO_MAX_CLUSTER_SIZE		0x00400000U
#define	SCO_CHECKSUM_CRC32C		1
#define	SCO_MAP_ENTRY_SIZE		16

#define	SCO_INCOMPAT_ALLOC_MAP_V1	(1U << 0)
#define	SCO_INCOMPAT_ZERO_DISCARD	(1U << 1)
#define	SCO_INCOMPAT_SUPPORTED \
	(SCO_INCOMPAT_ALLOC_MAP_V1 | SCO_INCOMPAT_ZERO_DISCARD)
#define	SCO_RO_COMPAT_SEALED		(1U << 0)
#define	SCO_RO_COMPAT_SUPPORTED		SCO_RO_COMPAT_SEALED
#define	SCO_COMPAT_IMAGE_DIGEST		(1U << 0)
#define	SCO_COMPAT_SUPPORTED		SCO_COMPAT_IMAGE_DIGEST

struct sco_superblock_decoded {
	uint64_t generation;
	uint64_t virtual_size;
	uint32_t logical_sector_size;
	uint32_t physical_sector_size;
	uint32_t cluster_size;
	uint64_t cluster_count;
	uint64_t data_area_offset;
	uint64_t base_descriptor_offset;
	uint32_t base_descriptor_length;
	uint64_t map_root_offset;
	uint32_t map_root_length;
	uint32_t incompatible_features;
	uint32_t readonly_compatible_features;
	uint32_t compatible_features;
	uint8_t image_uuid[16];
	uint8_t image_digest[32];
	bool has_image_digest;
};

struct sco_image {
	int fd;
	bool readonly;
	struct sco_superblock_decoded sb;
};

static uint32_t
sco_crc32c(const void *buf, size_t len)
{
	const uint8_t *p;
	uint32_t crc;
	size_t i;
	int bit;

	p = buf;
	crc = 0xffffffffU;
	for (i = 0; i < len; i++) {
		crc ^= p[i];
		for (bit = 0; bit < 8; bit++) {
			if ((crc & 1) != 0)
				crc = (crc >> 1) ^ 0x82f63b78U;
			else
				crc >>= 1;
		}
	}
	return (~crc);
}

static bool
is_power_of_two_u64(uint64_t value)
{
	return (value != 0 && (value & (value - 1)) == 0);
}

static bool
range_fits(uint64_t offset, uint64_t length, uint64_t limit)
{
	if (length > UINT64_MAX - offset)
		return (false);
	return (offset + length <= limit);
}

static bool
range_before_data(uint64_t offset, uint64_t length, uint64_t data_area_offset)
{
	return (offset >= SCO_METADATA_AREA_OFFSET &&
	    (offset % SCO_METADATA_PAGE_SIZE) == 0 &&
	    length != 0 && (length % SCO_METADATA_PAGE_SIZE) == 0 &&
	    range_fits(offset, length, data_area_offset));
}

static bool
reserved_zero(const uint8_t *buf, size_t offset, size_t length)
{
	size_t i;

	for (i = 0; i < length; i++) {
		if (buf[offset + i] != 0)
			return (false);
	}
	return (true);
}

static int
read_exact(int fd, uint64_t offset, void *buf, size_t len)
{
	ssize_t n;

	n = pread(fd, buf, len, (off_t)offset);
	if (n < 0)
		return (errno);
	if ((size_t)n != len)
		return (EIO);
	return (0);
}

static bool
crc32c_valid(uint8_t *buf, size_t len, size_t checksum_offset,
    uint32_t expected)
{
	uint8_t saved[4];
	uint32_t actual;

	memcpy(saved, buf + checksum_offset, sizeof(saved));
	memset(buf + checksum_offset, 0, sizeof(saved));
	actual = sco_crc32c(buf, len);
	memcpy(buf + checksum_offset, saved, sizeof(saved));
	return (actual == expected);
}

static int
sco_read_file_id(int fd, uint8_t image_uuid[16])
{
	uint8_t buf[SCO_FILE_ID_SIZE];
	uint32_t checksum_type, expected_crc, block_size;
	uint16_t major;
	int error;

	error = read_exact(fd, SCO_FILE_ID_OFFSET, buf, sizeof(buf));
	if (error != 0)
		return (error);
	if (memcmp(buf, SCO_MAGIC, SCO_MAGIC_SIZE) != 0)
		return (EINVAL);
	major = le16dec(buf + 0x0008);
	if (major != SCO_FORMAT_MAJOR)
		return (EINVAL);
	block_size = le32dec(buf + 0x000c);
	if (block_size != SCO_FILE_ID_SIZE)
		return (EINVAL);
	checksum_type = le32dec(buf + 0x0020);
	if (checksum_type != SCO_CHECKSUM_CRC32C)
		return (EINVAL);
	expected_crc = le32dec(buf + 0x0024);
	if (!crc32c_valid(buf, sizeof(buf), 0x0024, expected_crc))
		return (EINVAL);
	if (!reserved_zero(buf, 0x0028, sizeof(buf) - 0x0028))
		return (EINVAL);
	memcpy(image_uuid, buf + 0x0010, 16);
	return (0);
}

static bool
sco_superblock_layout_valid(const struct sco_superblock_decoded *sb,
    uint64_t file_size)
{
	uint64_t expected_clusters;

	if (sb->virtual_size == 0 || sb->logical_sector_size == 0 ||
	    sb->cluster_size == 0 || sb->cluster_count == 0)
		return (false);
	if (!is_power_of_two_u64(sb->logical_sector_size))
		return (false);
	if (sb->physical_sector_size != 0 &&
	    (!is_power_of_two_u64(sb->physical_sector_size) ||
	    sb->physical_sector_size < sb->logical_sector_size))
		return (false);
	if (sb->cluster_size < SCO_MIN_CLUSTER_SIZE ||
	    sb->cluster_size > SCO_MAX_CLUSTER_SIZE ||
	    !is_power_of_two_u64(sb->cluster_size) ||
	    (sb->cluster_size % sb->logical_sector_size) != 0)
		return (false);
	expected_clusters =
	    (sb->virtual_size + sb->cluster_size - 1) / sb->cluster_size;
	if (sb->cluster_count != expected_clusters)
		return (false);
	if (sb->data_area_offset < SCO_METADATA_AREA_OFFSET ||
	    (sb->data_area_offset % sb->cluster_size) != 0 ||
	    sb->data_area_offset > file_size)
		return (false);
	if (!range_before_data(sb->map_root_offset, sb->map_root_length,
	    sb->data_area_offset))
		return (false);
	if (sb->base_descriptor_offset == 0) {
		if (sb->base_descriptor_length != 0)
			return (false);
	} else if (!range_before_data(sb->base_descriptor_offset,
	    sb->base_descriptor_length, sb->data_area_offset)) {
		return (false);
	}
	return (true);
}

static int
sco_read_superblock(int fd, uint64_t offset, bool readonly,
    const uint8_t file_id_uuid[16], uint64_t file_size,
    struct sco_superblock_decoded *out)
{
	struct sco_superblock_decoded sb;
	uint8_t buf[SCO_SUPERBLOCK_SIZE];
	uint32_t checksum_type, expected_crc, superblock_size;
	uint32_t map_entry_size, has_image_digest;
	uint16_t major;
	int error;

	error = read_exact(fd, offset, buf, sizeof(buf));
	if (error != 0)
		return (error);
	if (memcmp(buf, SCO_MAGIC, SCO_MAGIC_SIZE) != 0)
		return (EINVAL);
	major = le16dec(buf + 0x0008);
	if (major != SCO_FORMAT_MAJOR)
		return (EINVAL);
	superblock_size = le32dec(buf + 0x000c);
	if (superblock_size != SCO_SUPERBLOCK_SIZE)
		return (EINVAL);
	checksum_type = le32dec(buf + 0x0010);
	if (checksum_type != SCO_CHECKSUM_CRC32C)
		return (EINVAL);
	expected_crc = le32dec(buf + 0x0014);
	if (!crc32c_valid(buf, sizeof(buf), 0x0014, expected_crc))
		return (EINVAL);
	if (le64dec(buf + 0x0040) != SCO_METADATA_AREA_OFFSET)
		return (EINVAL);
	map_entry_size = le32dec(buf + 0x006c);
	if (map_entry_size != SCO_MAP_ENTRY_SIZE)
		return (EINVAL);
	if (le32dec(buf + 0x005c) != 0 || le32dec(buf + 0x007c) != 0 ||
	    le32dec(buf + 0x00b4) != 0)
		return (EINVAL);
	if (!reserved_zero(buf, 0x00b8, sizeof(buf) - 0x00b8))
		return (EINVAL);

	memset(&sb, 0, sizeof(sb));
	sb.generation = le64dec(buf + 0x0018);
	sb.virtual_size = le64dec(buf + 0x0020);
	sb.logical_sector_size = le32dec(buf + 0x0028);
	sb.physical_sector_size = le32dec(buf + 0x002c);
	sb.cluster_size = le32dec(buf + 0x0030);
	sb.cluster_count = le64dec(buf + 0x0038);
	sb.data_area_offset = le64dec(buf + 0x0048);
	sb.base_descriptor_offset = le64dec(buf + 0x0050);
	sb.base_descriptor_length = le32dec(buf + 0x0058);
	sb.map_root_offset = le64dec(buf + 0x0060);
	sb.map_root_length = le32dec(buf + 0x0068);
	sb.incompatible_features = le32dec(buf + 0x0070);
	sb.readonly_compatible_features = le32dec(buf + 0x0074);
	sb.compatible_features = le32dec(buf + 0x0078);
	memcpy(sb.image_uuid, buf + 0x0080, 16);
	memcpy(sb.image_digest, buf + 0x0090, 32);
	has_image_digest = le32dec(buf + 0x00b0);
	if (has_image_digest > 1)
		return (EINVAL);
	sb.has_image_digest = has_image_digest != 0;

	if (memcmp(sb.image_uuid, file_id_uuid, 16) != 0)
		return (EINVAL);
	if ((sb.incompatible_features & ~SCO_INCOMPAT_SUPPORTED) != 0)
		return (EINVAL);
	if ((sb.readonly_compatible_features & ~SCO_RO_COMPAT_SUPPORTED) != 0 &&
	    !readonly)
		return (EINVAL);
	if ((sb.compatible_features & ~SCO_COMPAT_SUPPORTED) != 0) {
		/* Unknown compatible bits are explicitly ignored. */
	}
	if ((sb.incompatible_features & SCO_INCOMPAT_ALLOC_MAP_V1) == 0)
		return (EINVAL);
	if (!sco_superblock_layout_valid(&sb, file_size))
		return (EINVAL);

	*out = sb;
	return (0);
}

static int
sco_select_superblock(int fd, bool readonly, const uint8_t file_id_uuid[16],
    uint64_t file_size, struct sco_superblock_decoded *out)
{
	struct sco_superblock_decoded a, b;
	int error_a, error_b;

	error_a = sco_read_superblock(fd, SCO_SUPERBLOCK_A_OFFSET, readonly,
	    file_id_uuid, file_size, &a);
	error_b = sco_read_superblock(fd, SCO_SUPERBLOCK_B_OFFSET, readonly,
	    file_id_uuid, file_size, &b);
	if (error_a != 0 && error_b != 0)
		return (EINVAL);
	if (error_a == 0 && error_b != 0) {
		*out = a;
		return (0);
	}
	if (error_a != 0 && error_b == 0) {
		*out = b;
		return (0);
	}
	*out = b.generation > a.generation ? b : a;
	return (0);
}

static int
sco_probe(int fd, uint32_t *score)
{
	char magic[SCO_MAGIC_SIZE];
	ssize_t n;

	if (fd < 0 || score == NULL)
		return (EINVAL);
	n = pread(fd, magic, sizeof(magic), SCO_FILE_ID_OFFSET);
	if (n < 0)
		return (errno);
	*score = n == (ssize_t)sizeof(magic) &&
	    memcmp(magic, SCO_MAGIC, sizeof(magic)) == 0 ? 100 : 0;
	return (0);
}

static int
sco_open(const char *path __attribute__((unused)), int fd, bool readonly,
    void **statep)
{
	struct sco_image *sco;
	struct stat sb;
	uint8_t file_id_uuid[16];
	uint64_t file_size;
	int error;

	if (fd < 0 || statep == NULL)
		return (EINVAL);
	if (!readonly)
		return (EROFS);
	if (fstat(fd, &sb) != 0)
		return (errno);
	if (sb.st_size < (off_t)SCO_MIN_FILE_SIZE)
		return (EINVAL);
	file_size = (uint64_t)sb.st_size;

	error = sco_read_file_id(fd, file_id_uuid);
	if (error != 0)
		return (error);

	sco = calloc(1, sizeof(*sco));
	if (sco == NULL)
		return (ENOMEM);
	sco->fd = fd;
	sco->readonly = readonly;
	error = sco_select_superblock(fd, readonly, file_id_uuid, file_size,
	    &sco->sb);
	if (error != 0) {
		free(sco);
		return (error);
	}
	*statep = sco;
	return (0);
}

static int
sco_get_info(void *statep, struct scorpi_image_info *info)
{
	struct sco_image *sco;

	if (statep == NULL || info == NULL)
		return (EINVAL);
	sco = statep;
	*info = (struct scorpi_image_info){
		.format = SCORPI_IMAGE_FORMAT_SCO,
		.virtual_size = sco->sb.virtual_size,
		.logical_sector_size = sco->sb.logical_sector_size,
		.physical_sector_size = sco->sb.physical_sector_size,
		.cluster_size = sco->sb.cluster_size,
		.readonly = sco->readonly,
		.sealed = (sco->sb.readonly_compatible_features &
		    SCO_RO_COMPAT_SEALED) != 0,
		.has_image_digest = sco->sb.has_image_digest,
	};
	if (sco->sb.has_image_digest)
		memcpy(info->image_digest, sco->sb.image_digest,
		    sizeof(info->image_digest));
	return (0);
}

static int
sco_map(void *state __attribute__((unused)),
    uint64_t offset __attribute__((unused)),
    uint64_t length __attribute__((unused)),
    struct scorpi_image_extent *extent __attribute__((unused)))
{
	return (ENOTSUP);
}

static int
sco_read(void *state __attribute__((unused)), void *buf __attribute__((unused)),
    uint64_t offset __attribute__((unused)),
    size_t len __attribute__((unused)))
{
	return (ENOTSUP);
}

static int
sco_write(void *state __attribute__((unused)),
    const void *buf __attribute__((unused)),
    uint64_t offset __attribute__((unused)),
    size_t len __attribute__((unused)))
{
	return (EROFS);
}

static int
sco_discard(void *state __attribute__((unused)),
    uint64_t offset __attribute__((unused)),
    uint64_t length __attribute__((unused)))
{
	return (EROFS);
}

static int
sco_flush(void *state __attribute__((unused)))
{
	return (0);
}

static int
sco_close(void *statep)
{
	struct sco_image *sco;
	int error;

	if (statep == NULL)
		return (0);
	sco = statep;
	error = 0;
	if (sco->fd >= 0 && close(sco->fd) != 0)
		error = errno;
	free(sco);
	return (error);
}

static const struct scorpi_image_ops scorpi_sco_ops = {
	.name = "sco",
	.probe = sco_probe,
	.open = sco_open,
	.get_info = sco_get_info,
	.map = sco_map,
	.read = sco_read,
	.write = sco_write,
	.discard = sco_discard,
	.flush = sco_flush,
	.close = sco_close,
};

const struct scorpi_image_ops *
scorpi_image_sco_backend(void)
{
	return (&scorpi_sco_ops);
}
