/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/errno.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <support/endian.h>

#include "scorpi_image.h"

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
#define	SCO_BASE_DESCRIPTOR_MIN_SIZE	0x00000050U
#define	SCO_CHECKSUM_CRC32C		1
#define	SCO_MAP_ENTRY_SIZE		16
#define	SCO_MAP_PAGE_HEADER_SIZE		0x00000018U
#define	SCO_MAP_ENTRIES_PER_PAGE \
	((SCO_METADATA_PAGE_SIZE - SCO_MAP_PAGE_HEADER_SIZE) / \
	SCO_MAP_ENTRY_SIZE)

#define	SCO_INCOMPAT_ALLOC_MAP_V1	(1U << 0)
#define	SCO_INCOMPAT_ZERO_DISCARD	(1U << 1)
#define	SCO_INCOMPAT_SUPPORTED \
	(SCO_INCOMPAT_ALLOC_MAP_V1 | SCO_INCOMPAT_ZERO_DISCARD)
#define	SCO_RO_COMPAT_SEALED		(1U << 0)
#define	SCO_RO_COMPAT_SUPPORTED		SCO_RO_COMPAT_SEALED
#define	SCO_COMPAT_IMAGE_DIGEST		(1U << 0)
#define	SCO_COMPAT_SUPPORTED		SCO_COMPAT_IMAGE_DIGEST

#define	SCO_MAP_STATE_ABSENT		0
#define	SCO_MAP_STATE_PRESENT		1
#define	SCO_MAP_STATE_ZERO		2
#define	SCO_MAP_STATE_DISCARDED		3

#define	SCO_DATA_LOCK_STRIPES		64

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
	pthread_rwlock_t lock;
	pthread_rwlock_t data_locks[SCO_DATA_LOCK_STRIPES];
	bool readonly;
	uint64_t file_size;
	uint64_t active_superblock_offset;
	struct sco_superblock_decoded sb;
	bool has_base;
	char *base_uri;
	uint8_t base_uuid[16];
	bool has_base_uuid;
	uint8_t base_digest[32];
	bool has_base_digest;
};

struct sco_lookup {
	struct scorpi_image_extent extent;
	uint64_t physical_offset;
};

struct sco_root_entry_ref {
	uint8_t page[SCO_METADATA_PAGE_SIZE];
	uint64_t page_offset;
	uint64_t entry_offset;
	uint64_t map_page_offset;
	uint32_t map_page_crc32c;
};

static int
sco_data_locks_init(struct sco_image *sco)
{
	size_t i, j;
	int error;

	if (sco == NULL)
		return (EINVAL);
	for (i = 0; i < SCO_DATA_LOCK_STRIPES; i++) {
		error = pthread_rwlock_init(&sco->data_locks[i], NULL);
		if (error != 0) {
			for (j = 0; j < i; j++)
				pthread_rwlock_destroy(&sco->data_locks[j]);
			return (error);
		}
	}
	return (0);
}

static void
sco_data_locks_destroy(struct sco_image *sco)
{
	size_t i;

	if (sco == NULL)
		return;
	for (i = 0; i < SCO_DATA_LOCK_STRIPES; i++)
		pthread_rwlock_destroy(&sco->data_locks[i]);
}

static pthread_rwlock_t *
sco_data_lock_for_cluster(struct sco_image *sco, uint64_t cluster_index)
{
	return (&sco->data_locks[cluster_index % SCO_DATA_LOCK_STRIPES]);
}

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

static bool
buffer_all_zero(const void *buf, size_t len)
{
	const uint8_t *p;
	size_t i;

	p = buf;
	for (i = 0; i < len; i++) {
		if (p[i] != 0)
			return (false);
	}
	return (true);
}

static int
read_exact(int fd, uint64_t offset, void *buf, size_t len)
{
	uint8_t *p;
	ssize_t n;

	p = buf;
	while (len > 0) {
		n = pread(fd, p, len, (off_t)offset);
		if (n < 0)
			return (errno);
		if (n == 0)
			return (EIO);
		p += n;
		offset += (uint64_t)n;
		len -= (size_t)n;
	}
	return (0);
}

static int
write_exact(int fd, uint64_t offset, const void *buf, size_t len)
{
	const uint8_t *p;
	ssize_t n;

	p = buf;
	while (len > 0) {
		n = pwrite(fd, p, len, (off_t)offset);
		if (n < 0)
			return (errno);
		if (n == 0)
			return (EIO);
		p += n;
		offset += (uint64_t)n;
		len -= (size_t)n;
	}
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

static uint32_t
page_crc32c_update(uint8_t page[SCO_METADATA_PAGE_SIZE])
{
	uint32_t crc;

	le32enc(page + 0x0008, 0);
	crc = sco_crc32c(page, SCO_METADATA_PAGE_SIZE);
	le32enc(page + 0x0008, crc);
	return (crc);
}

static uint64_t
sco_inactive_superblock_offset(uint64_t active_offset)
{
	return (active_offset == SCO_SUPERBLOCK_A_OFFSET ?
	    SCO_SUPERBLOCK_B_OFFSET : SCO_SUPERBLOCK_A_OFFSET);
}

static void
sco_build_superblock(uint8_t buf[SCO_SUPERBLOCK_SIZE],
    const struct sco_superblock_decoded *sb)
{
	uint32_t crc;

	memset(buf, 0, SCO_SUPERBLOCK_SIZE);
	memcpy(buf, SCO_MAGIC, SCO_MAGIC_SIZE);
	le16enc(buf + 0x0008, SCO_FORMAT_MAJOR);
	le16enc(buf + 0x000a, 0);
	le32enc(buf + 0x000c, SCO_SUPERBLOCK_SIZE);
	le32enc(buf + 0x0010, SCO_CHECKSUM_CRC32C);
	le64enc(buf + 0x0018, sb->generation);
	le64enc(buf + 0x0020, sb->virtual_size);
	le32enc(buf + 0x0028, sb->logical_sector_size);
	le32enc(buf + 0x002c, sb->physical_sector_size);
	le32enc(buf + 0x0030, sb->cluster_size);
	le64enc(buf + 0x0038, sb->cluster_count);
	le64enc(buf + 0x0040, SCO_METADATA_AREA_OFFSET);
	le64enc(buf + 0x0048, sb->data_area_offset);
	le64enc(buf + 0x0050, sb->base_descriptor_offset);
	le32enc(buf + 0x0058, sb->base_descriptor_length);
	le32enc(buf + 0x005c, 0);
	le64enc(buf + 0x0060, sb->map_root_offset);
	le32enc(buf + 0x0068, sb->map_root_length);
	le32enc(buf + 0x006c, SCO_MAP_ENTRY_SIZE);
	le32enc(buf + 0x0070, sb->incompatible_features);
	le32enc(buf + 0x0074, sb->readonly_compatible_features);
	le32enc(buf + 0x0078, sb->compatible_features);
	le32enc(buf + 0x007c, 0);
	memcpy(buf + 0x0080, sb->image_uuid, sizeof(sb->image_uuid));
	if (sb->has_image_digest)
		memcpy(buf + 0x0090, sb->image_digest,
		    sizeof(sb->image_digest));
	le32enc(buf + 0x00b0, sb->has_image_digest ? 1 : 0);
	le32enc(buf + 0x00b4, 0);
	crc = sco_crc32c(buf, SCO_SUPERBLOCK_SIZE);
	le32enc(buf + 0x0014, crc);
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
    uint64_t file_size, struct sco_superblock_decoded *out,
    uint64_t *active_offsetp)
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
		if (active_offsetp != NULL)
			*active_offsetp = SCO_SUPERBLOCK_A_OFFSET;
		return (0);
	}
	if (error_a != 0 && error_b == 0) {
		*out = b;
		if (active_offsetp != NULL)
			*active_offsetp = SCO_SUPERBLOCK_B_OFFSET;
		return (0);
	}
	if (b.generation > a.generation) {
		*out = b;
		if (active_offsetp != NULL)
			*active_offsetp = SCO_SUPERBLOCK_B_OFFSET;
	} else {
		*out = a;
		if (active_offsetp != NULL)
			*active_offsetp = SCO_SUPERBLOCK_A_OFFSET;
	}
	return (0);
}

static int
sco_read_base_descriptor(struct sco_image *sco)
{
	uint8_t *buf;
	uint32_t descriptor_size, checksum_type, expected_crc;
	uint32_t flags, base_uri_length, reserved0;
	uint32_t has_base_uuid, has_base_digest;
	size_t padding_offset;
	char *base_uri;
	int error;

	if (sco->sb.base_descriptor_offset == 0)
		return (0);

	buf = malloc(sco->sb.base_descriptor_length);
	if (buf == NULL)
		return (ENOMEM);
	error = read_exact(sco->fd, sco->sb.base_descriptor_offset, buf,
	    sco->sb.base_descriptor_length);
	if (error != 0) {
		free(buf);
		return (error);
	}

	descriptor_size = le32dec(buf + 0x0000);
	checksum_type = le32dec(buf + 0x0004);
	expected_crc = le32dec(buf + 0x0008);
	flags = le32dec(buf + 0x000c);
	base_uri_length = le32dec(buf + 0x0010);
	reserved0 = le32dec(buf + 0x0014);
	has_base_uuid = le32dec(buf + 0x0048);
	has_base_digest = le32dec(buf + 0x004c);

	if (descriptor_size != sco->sb.base_descriptor_length ||
	    descriptor_size < SCO_BASE_DESCRIPTOR_MIN_SIZE ||
	    (descriptor_size % SCO_METADATA_PAGE_SIZE) != 0 ||
	    checksum_type != SCO_CHECKSUM_CRC32C ||
	    flags != 0 || reserved0 != 0 ||
	    has_base_uuid > 1 || has_base_digest > 1 ||
	    base_uri_length == 0 ||
	    base_uri_length > descriptor_size - SCO_BASE_DESCRIPTOR_MIN_SIZE) {
		free(buf);
		return (EINVAL);
	}
	if (!crc32c_valid(buf, descriptor_size, 0x0008, expected_crc)) {
		free(buf);
		return (EINVAL);
	}
	padding_offset = SCO_BASE_DESCRIPTOR_MIN_SIZE + base_uri_length;
	if (!reserved_zero(buf, padding_offset, descriptor_size -
	    padding_offset)) {
		free(buf);
		return (EINVAL);
	}
	if (base_uri_length < 5 ||
	    memcmp(buf + SCO_BASE_DESCRIPTOR_MIN_SIZE, "file:", 5) != 0) {
		free(buf);
		return (ENOTSUP);
	}

	base_uri = calloc(1, (size_t)base_uri_length + 1);
	if (base_uri == NULL) {
		free(buf);
		return (ENOMEM);
	}
	memcpy(base_uri, buf + SCO_BASE_DESCRIPTOR_MIN_SIZE,
	    base_uri_length);

	sco->has_base = true;
	sco->base_uri = base_uri;
	if (has_base_uuid != 0) {
		memcpy(sco->base_uuid, buf + 0x0018, sizeof(sco->base_uuid));
		sco->has_base_uuid = true;
	}
	if (has_base_digest != 0) {
		memcpy(sco->base_digest, buf + 0x0028,
		    sizeof(sco->base_digest));
		sco->has_base_digest = true;
	}
	free(buf);
	return (0);
}

static uint64_t
sco_cluster_stored_length(const struct sco_image *sco, uint64_t cluster_index)
{
	uint64_t cluster_offset, remaining;

	cluster_offset = cluster_index * sco->sb.cluster_size;
	remaining = sco->sb.virtual_size - cluster_offset;
	if (remaining < sco->sb.cluster_size)
		return (remaining);
	return (sco->sb.cluster_size);
}

static int
sco_validate_map_page_header(uint8_t page[SCO_METADATA_PAGE_SIZE],
    uint32_t expected_crc, uint32_t *entry_countp, uint64_t *first_indexp)
{
	uint32_t page_size, checksum_type, page_crc, entry_count;

	page_size = le32dec(page + 0x0000);
	checksum_type = le32dec(page + 0x0004);
	page_crc = le32dec(page + 0x0008);
	entry_count = le32dec(page + 0x000c);
	if (page_size != SCO_METADATA_PAGE_SIZE ||
	    checksum_type != SCO_CHECKSUM_CRC32C ||
	    page_crc != expected_crc ||
	    entry_count > SCO_MAP_ENTRIES_PER_PAGE)
		return (EINVAL);
	if (!crc32c_valid(page, SCO_METADATA_PAGE_SIZE, 0x0008, page_crc))
		return (EINVAL);
	*entry_countp = entry_count;
	*first_indexp = le64dec(page + 0x0010);
	return (0);
}

static int
sco_find_root_entry(struct sco_image *sco, uint64_t root_index,
    uint64_t *map_page_offsetp, uint32_t *map_page_crc32cp)
{
	uint8_t page[SCO_METADATA_PAGE_SIZE];
	uint64_t page_offset, first_root_index, last_root_index;
	uint64_t root_page_count, i, entry_index;
	uint32_t entry_count, expected_crc, flags;
	int error;

	root_page_count = sco->sb.map_root_length / SCO_METADATA_PAGE_SIZE;
	for (i = 0; i < root_page_count; i++) {
		page_offset = sco->sb.map_root_offset +
		    i * SCO_METADATA_PAGE_SIZE;
		error = read_exact(sco->fd, page_offset, page, sizeof(page));
		if (error != 0)
			return (error);
		expected_crc = le32dec(page + 0x0008);
		error = sco_validate_map_page_header(page, expected_crc,
		    &entry_count, &first_root_index);
		if (error != 0)
			return (error);
		if (first_root_index + entry_count < first_root_index)
			return (EINVAL);
		if (!reserved_zero(page,
		    SCO_MAP_PAGE_HEADER_SIZE + entry_count * SCO_MAP_ENTRY_SIZE,
		    SCO_METADATA_PAGE_SIZE - SCO_MAP_PAGE_HEADER_SIZE -
		    entry_count * SCO_MAP_ENTRY_SIZE))
			return (EINVAL);
		last_root_index = first_root_index + entry_count;
		if (last_root_index < first_root_index)
			return (EINVAL);
		if (root_index < first_root_index || root_index >=
		    last_root_index)
			continue;
		entry_index = root_index - first_root_index;
		page_offset = SCO_MAP_PAGE_HEADER_SIZE +
		    entry_index * SCO_MAP_ENTRY_SIZE;
		flags = le32dec(page + page_offset + 0x000c);
		if (flags != 0)
			return (EINVAL);
		*map_page_offsetp = le64dec(page + page_offset + 0x0000);
		*map_page_crc32cp = le32dec(page + page_offset + 0x0008);
		return (0);
	}

	*map_page_offsetp = 0;
	*map_page_crc32cp = 0;
	return (0);
}

static int
sco_read_root_entry_ref(struct sco_image *sco, uint64_t root_index,
    struct sco_root_entry_ref *ref)
{
	uint64_t page_offset, first_root_index, last_root_index;
	uint64_t root_page_count, i, entry_index;
	uint32_t entry_count, expected_crc, flags;
	int error;

	if (sco == NULL || ref == NULL)
		return (EINVAL);

	root_page_count = sco->sb.map_root_length / SCO_METADATA_PAGE_SIZE;
	for (i = 0; i < root_page_count; i++) {
		page_offset = sco->sb.map_root_offset +
		    i * SCO_METADATA_PAGE_SIZE;
		error = read_exact(sco->fd, page_offset, ref->page,
		    sizeof(ref->page));
		if (error != 0)
			return (error);
		expected_crc = le32dec(ref->page + 0x0008);
		error = sco_validate_map_page_header(ref->page, expected_crc,
		    &entry_count, &first_root_index);
		if (error != 0)
			return (error);
		if (first_root_index + entry_count < first_root_index)
			return (EINVAL);
		if (!reserved_zero(ref->page,
		    SCO_MAP_PAGE_HEADER_SIZE + entry_count * SCO_MAP_ENTRY_SIZE,
		    SCO_METADATA_PAGE_SIZE - SCO_MAP_PAGE_HEADER_SIZE -
		    entry_count * SCO_MAP_ENTRY_SIZE))
			return (EINVAL);
		last_root_index = first_root_index + entry_count;
		if (last_root_index < first_root_index)
			return (EINVAL);
		if (root_index < first_root_index ||
		    root_index >= last_root_index)
			continue;

		entry_index = root_index - first_root_index;
		ref->page_offset = page_offset;
		ref->entry_offset = SCO_MAP_PAGE_HEADER_SIZE +
		    entry_index * SCO_MAP_ENTRY_SIZE;
		flags = le32dec(ref->page + ref->entry_offset + 0x000c);
		if (flags != 0)
			return (EINVAL);
		ref->map_page_offset = le64dec(ref->page +
		    ref->entry_offset + 0x0000);
		ref->map_page_crc32c = le32dec(ref->page +
		    ref->entry_offset + 0x0008);
		return (0);
	}
	return (ENOENT);
}

static bool
sco_metadata_fixed_range_used(const struct sco_image *sco, uint64_t offset)
{
	uint64_t end;

	end = sco->sb.map_root_offset + sco->sb.map_root_length;
	if (offset >= sco->sb.map_root_offset && offset < end)
		return (true);
	if (sco->sb.base_descriptor_offset == 0)
		return (false);
	end = sco->sb.base_descriptor_offset + sco->sb.base_descriptor_length;
	return (offset >= sco->sb.base_descriptor_offset && offset < end);
}

static int
sco_metadata_page_used(struct sco_image *sco, uint64_t offset, bool *usedp)
{
	uint8_t page[SCO_METADATA_PAGE_SIZE];
	uint64_t page_offset, first_root_index;
	uint64_t root_page_count, i, j, map_page_offset;
	uint32_t entry_count, expected_crc, flags;
	int error;

	if (sco == NULL || usedp == NULL)
		return (EINVAL);
	if (sco_metadata_fixed_range_used(sco, offset)) {
		*usedp = true;
		return (0);
	}

	root_page_count = sco->sb.map_root_length / SCO_METADATA_PAGE_SIZE;
	for (i = 0; i < root_page_count; i++) {
		page_offset = sco->sb.map_root_offset +
		    i * SCO_METADATA_PAGE_SIZE;
		error = read_exact(sco->fd, page_offset, page, sizeof(page));
		if (error != 0)
			return (error);
		expected_crc = le32dec(page + 0x0008);
		error = sco_validate_map_page_header(page, expected_crc,
		    &entry_count, &first_root_index);
		if (error != 0)
			return (error);
		if (first_root_index + entry_count < first_root_index)
			return (EINVAL);
		if (!reserved_zero(page,
		    SCO_MAP_PAGE_HEADER_SIZE + entry_count * SCO_MAP_ENTRY_SIZE,
		    SCO_METADATA_PAGE_SIZE - SCO_MAP_PAGE_HEADER_SIZE -
		    entry_count * SCO_MAP_ENTRY_SIZE))
			return (EINVAL);
		for (j = 0; j < entry_count; j++) {
			page_offset = SCO_MAP_PAGE_HEADER_SIZE +
			    j * SCO_MAP_ENTRY_SIZE;
			flags = le32dec(page + page_offset + 0x000c);
			if (flags != 0)
				return (EINVAL);
			map_page_offset = le64dec(page + page_offset);
			if (map_page_offset == 0)
				continue;
			if (!range_before_data(map_page_offset,
			    SCO_METADATA_PAGE_SIZE, sco->sb.data_area_offset))
				return (EINVAL);
			if (map_page_offset == offset) {
				*usedp = true;
				return (0);
			}
		}
	}

	*usedp = false;
	return (0);
}

static bool
sco_offset_reserved(uint64_t offset, const uint64_t *reserved,
    size_t reserved_count)
{
	size_t i;

	for (i = 0; i < reserved_count; i++) {
		if (reserved[i] == offset)
			return (true);
	}
	return (false);
}

static int
sco_alloc_metadata_page_reserved(struct sco_image *sco, uint64_t *offsetp,
    const uint64_t *reserved, size_t reserved_count)
{
	uint64_t offset;
	bool used;
	int error;

	if (sco == NULL || offsetp == NULL)
		return (EINVAL);
	for (offset = SCO_METADATA_AREA_OFFSET;
	    offset + SCO_METADATA_PAGE_SIZE <= sco->sb.data_area_offset;
	    offset += SCO_METADATA_PAGE_SIZE) {
		if (sco_offset_reserved(offset, reserved, reserved_count))
			continue;
		error = sco_metadata_page_used(sco, offset, &used);
		if (error != 0)
			return (error);
		if (!used) {
			*offsetp = offset;
			return (0);
		}
	}
	return (ENOSPC);
}

static int
sco_alloc_metadata_run_reserved(struct sco_image *sco, size_t page_count,
    uint64_t *offsetp, const uint64_t *reserved, size_t reserved_count)
{
	uint64_t offset, candidate;
	size_t i;
	bool used;
	int error;

	if (sco == NULL || offsetp == NULL || page_count == 0)
		return (EINVAL);
	for (offset = SCO_METADATA_AREA_OFFSET;
	    offset + page_count * SCO_METADATA_PAGE_SIZE <=
	    sco->sb.data_area_offset; offset += SCO_METADATA_PAGE_SIZE) {
		for (i = 0; i < page_count; i++) {
			candidate = offset + i * SCO_METADATA_PAGE_SIZE;
			if (sco_offset_reserved(candidate, reserved,
			    reserved_count))
				break;
			error = sco_metadata_page_used(sco, candidate, &used);
			if (error != 0)
				return (error);
			if (used)
				break;
		}
		if (i == page_count) {
			*offsetp = offset;
			return (0);
		}
	}
	return (ENOSPC);
}

static int
sco_lookup_extent(struct sco_image *sco, uint64_t offset, uint64_t length,
    struct sco_lookup *lookup)
{
	uint8_t page[SCO_METADATA_PAGE_SIZE];
	uint64_t cluster_index, root_index, entry_index, map_page_offset;
	uint64_t first_cluster_index, last_cluster_index, physical_offset;
	uint64_t cluster_offset, cluster_remaining, covered;
	uint32_t map_page_crc32c, entry_count, reserved1;
	uint16_t reserved0;
	uint8_t state, flags;
	int error;

	if (sco == NULL || lookup == NULL || length == 0)
		return (EINVAL);
	if (offset >= sco->sb.virtual_size)
		return (EINVAL);
	if (length > sco->sb.virtual_size - offset)
		length = sco->sb.virtual_size - offset;

	cluster_index = offset / sco->sb.cluster_size;
	cluster_offset = offset % sco->sb.cluster_size;
	cluster_remaining = sco_cluster_stored_length(sco, cluster_index) -
	    cluster_offset;
	covered = length < cluster_remaining ? length : cluster_remaining;
	root_index = cluster_index / SCO_MAP_ENTRIES_PER_PAGE;
	entry_index = cluster_index % SCO_MAP_ENTRIES_PER_PAGE;

	error = sco_find_root_entry(sco, root_index, &map_page_offset,
	    &map_page_crc32c);
	if (error != 0)
		return (error);
	if (map_page_offset == 0) {
		covered = (SCO_MAP_ENTRIES_PER_PAGE - entry_index) *
		    (uint64_t)sco->sb.cluster_size - cluster_offset;
		if (covered > length)
			covered = length;
		*lookup = (struct sco_lookup){
			.extent = {
				.offset = offset,
				.length = covered,
				.state = SCORPI_IMAGE_EXTENT_ABSENT,
			},
		};
		return (0);
	}
	if (!range_before_data(map_page_offset, SCO_METADATA_PAGE_SIZE,
	    sco->sb.data_area_offset))
		return (EINVAL);

	error = read_exact(sco->fd, map_page_offset, page, sizeof(page));
	if (error != 0)
		return (error);
	error = sco_validate_map_page_header(page, map_page_crc32c,
	    &entry_count, &first_cluster_index);
	if (error != 0)
		return (error);
	if (!reserved_zero(page,
	    SCO_MAP_PAGE_HEADER_SIZE + entry_count * SCO_MAP_ENTRY_SIZE,
	    SCO_METADATA_PAGE_SIZE - SCO_MAP_PAGE_HEADER_SIZE -
	    entry_count * SCO_MAP_ENTRY_SIZE))
		return (EINVAL);
	last_cluster_index = first_cluster_index + entry_count;
	if (last_cluster_index < first_cluster_index ||
	    cluster_index < first_cluster_index ||
	    cluster_index >= last_cluster_index)
		return (EINVAL);

	entry_index = cluster_index - first_cluster_index;
	entry_index = SCO_MAP_PAGE_HEADER_SIZE +
	    entry_index * SCO_MAP_ENTRY_SIZE;
	state = page[entry_index + 0x0000];
	flags = page[entry_index + 0x0001];
	reserved0 = le16dec(page + entry_index + 0x0002);
	reserved1 = le32dec(page + entry_index + 0x0004);
	physical_offset = le64dec(page + entry_index + 0x0008);
	if (flags != 0 || reserved0 != 0 || reserved1 != 0)
		return (EINVAL);

	switch (state) {
	case SCO_MAP_STATE_ABSENT:
		if (physical_offset != 0)
			return (EINVAL);
		lookup->extent.state = SCORPI_IMAGE_EXTENT_ABSENT;
		break;
	case SCO_MAP_STATE_PRESENT:
		if (physical_offset < sco->sb.data_area_offset ||
		    (physical_offset % sco->sb.cluster_size) != 0 ||
		    !range_fits(physical_offset,
		    sco_cluster_stored_length(sco, cluster_index),
		    sco->file_size))
			return (EINVAL);
		lookup->extent.state = SCORPI_IMAGE_EXTENT_PRESENT;
		lookup->physical_offset = physical_offset;
		break;
	case SCO_MAP_STATE_ZERO:
	case SCO_MAP_STATE_DISCARDED:
		if ((sco->sb.incompatible_features &
		    SCO_INCOMPAT_ZERO_DISCARD) == 0 || physical_offset != 0)
			return (EINVAL);
		lookup->extent.state = state == SCO_MAP_STATE_ZERO ?
		    SCORPI_IMAGE_EXTENT_ZERO :
		    SCORPI_IMAGE_EXTENT_DISCARDED;
		break;
	default:
		return (EINVAL);
	}
	lookup->extent.offset = offset;
	lookup->extent.length = covered;
	return (0);
}

static uint64_t
align_up_u64(uint64_t value, uint64_t alignment)
{
	return ((value + alignment - 1) & ~(alignment - 1));
}

static int
sco_alloc_data_cluster(struct sco_image *sco, uint64_t cluster_index,
    const void *buf, uint64_t *physical_offsetp)
{
	uint64_t physical_offset, stored_length, end;
	int error;

	if (sco == NULL || buf == NULL || physical_offsetp == NULL)
		return (EINVAL);
	stored_length = sco_cluster_stored_length(sco, cluster_index);
	if (stored_length == 0 || stored_length > SIZE_MAX)
		return (EINVAL);
	if (sco->file_size > UINT64_MAX - sco->sb.cluster_size + 1)
		return (EFBIG);
	physical_offset = align_up_u64(sco->file_size, sco->sb.cluster_size);
	if (physical_offset < sco->sb.data_area_offset)
		physical_offset = sco->sb.data_area_offset;
	if (stored_length > UINT64_MAX - physical_offset)
		return (EFBIG);
	end = physical_offset + stored_length;
	error = write_exact(sco->fd, physical_offset, buf,
	    (size_t)stored_length);
	if (error != 0)
		return (error);
	if (end > sco->file_size)
		sco->file_size = end;
	*physical_offsetp = physical_offset;
	return (0);
}

static uint32_t
sco_map_entry_count_for_root(const struct sco_image *sco, uint64_t root_index)
{
	uint64_t first_cluster_index, remaining;

	first_cluster_index = root_index * SCO_MAP_ENTRIES_PER_PAGE;
	if (first_cluster_index >= sco->sb.cluster_count)
		return (0);
	remaining = sco->sb.cluster_count - first_cluster_index;
	if (remaining > SCO_MAP_ENTRIES_PER_PAGE)
		return (SCO_MAP_ENTRIES_PER_PAGE);
	return ((uint32_t)remaining);
}

static void
sco_build_empty_map_page(uint8_t page[SCO_METADATA_PAGE_SIZE],
    uint64_t root_index, uint32_t entry_count)
{
	memset(page, 0, SCO_METADATA_PAGE_SIZE);
	le32enc(page + 0x0000, SCO_METADATA_PAGE_SIZE);
	le32enc(page + 0x0004, SCO_CHECKSUM_CRC32C);
	le32enc(page + 0x000c, entry_count);
	le64enc(page + 0x0010, root_index * SCO_MAP_ENTRIES_PER_PAGE);
}

static int
sco_commit_map_entry(struct sco_image *sco, uint64_t cluster_index,
    uint8_t state, uint64_t physical_offset)
{
	struct sco_root_entry_ref ref;
	struct sco_superblock_decoded next_sb;
	uint8_t page[SCO_METADATA_PAGE_SIZE], superblock[SCO_SUPERBLOCK_SIZE];
	uint8_t *root;
	uint64_t root_index, first_cluster_index, last_cluster_index;
	uint64_t entry_index, entry_offset, map_page_offset;
	uint64_t new_map_page_offset, new_root_offset, inactive_offset;
	uint64_t reserved[2];
	size_t root_page_index, root_page_count;
	uint32_t entry_count, map_page_crc32c;
	int error;

	if (sco == NULL)
		return (EINVAL);
	root_index = cluster_index / SCO_MAP_ENTRIES_PER_PAGE;
	error = sco_read_root_entry_ref(sco, root_index, &ref);
	if (error != 0)
		return (error);

	if (ref.map_page_offset == 0) {
		if (ref.map_page_crc32c != 0)
			return (EINVAL);
		entry_count = sco_map_entry_count_for_root(sco, root_index);
		if (entry_count == 0)
			return (EINVAL);
		sco_build_empty_map_page(page, root_index, entry_count);
		first_cluster_index = root_index * SCO_MAP_ENTRIES_PER_PAGE;
	} else {
		map_page_offset = ref.map_page_offset;
		if (!range_before_data(map_page_offset, SCO_METADATA_PAGE_SIZE,
		    sco->sb.data_area_offset))
			return (EINVAL);
		error = read_exact(sco->fd, map_page_offset, page,
		    sizeof(page));
		if (error != 0)
			return (error);
		error = sco_validate_map_page_header(page,
		    ref.map_page_crc32c, &entry_count, &first_cluster_index);
		if (error != 0)
			return (error);
		if (!reserved_zero(page,
		    SCO_MAP_PAGE_HEADER_SIZE + entry_count * SCO_MAP_ENTRY_SIZE,
		    SCO_METADATA_PAGE_SIZE - SCO_MAP_PAGE_HEADER_SIZE -
		    entry_count * SCO_MAP_ENTRY_SIZE))
			return (EINVAL);
	}

	last_cluster_index = first_cluster_index + entry_count;
	if (last_cluster_index < first_cluster_index ||
	    cluster_index < first_cluster_index ||
	    cluster_index >= last_cluster_index)
		return (EINVAL);
	entry_index = cluster_index - first_cluster_index;
	entry_offset = SCO_MAP_PAGE_HEADER_SIZE +
	    entry_index * SCO_MAP_ENTRY_SIZE;
	page[entry_offset + 0x0000] = state;
	page[entry_offset + 0x0001] = 0;
	le16enc(page + entry_offset + 0x0002, 0);
	le32enc(page + entry_offset + 0x0004, 0);
	le64enc(page + entry_offset + 0x0008, physical_offset);
	map_page_crc32c = page_crc32c_update(page);

	reserved[0] = 0;
	error = sco_alloc_metadata_page_reserved(sco, &new_map_page_offset,
	    NULL, 0);
	if (error != 0)
		return (error);
	reserved[0] = new_map_page_offset;
	root_page_count = sco->sb.map_root_length / SCO_METADATA_PAGE_SIZE;
	error = sco_alloc_metadata_run_reserved(sco, root_page_count,
	    &new_root_offset, reserved, 1);
	if (error != 0)
		return (error);

	root = malloc(sco->sb.map_root_length);
	if (root == NULL)
		return (ENOMEM);
	error = read_exact(sco->fd, sco->sb.map_root_offset, root,
	    sco->sb.map_root_length);
	if (error != 0)
		goto out;
	root_page_index = (ref.page_offset - sco->sb.map_root_offset) /
	    SCO_METADATA_PAGE_SIZE;
	if (root_page_index >= root_page_count) {
		error = EINVAL;
		goto out;
	}
	le64enc(root + root_page_index * SCO_METADATA_PAGE_SIZE +
	    ref.entry_offset + 0x0000, new_map_page_offset);
	le32enc(root + root_page_index * SCO_METADATA_PAGE_SIZE +
	    ref.entry_offset + 0x0008, map_page_crc32c);
	le32enc(root + root_page_index * SCO_METADATA_PAGE_SIZE +
	    ref.entry_offset + 0x000c, 0);
	(void)page_crc32c_update(root +
	    root_page_index * SCO_METADATA_PAGE_SIZE);

	error = write_exact(sco->fd, new_map_page_offset, page, sizeof(page));
	if (error != 0)
		goto out;
	error = write_exact(sco->fd, new_root_offset, root,
	    sco->sb.map_root_length);
	if (error != 0)
		goto out;
	if (fsync(sco->fd) != 0) {
		error = errno;
		goto out;
	}

	next_sb = sco->sb;
	next_sb.generation++;
	next_sb.map_root_offset = new_root_offset;
	if (state == SCO_MAP_STATE_ZERO || state == SCO_MAP_STATE_DISCARDED)
		next_sb.incompatible_features |= SCO_INCOMPAT_ZERO_DISCARD;
	sco_build_superblock(superblock, &next_sb);
	inactive_offset =
	    sco_inactive_superblock_offset(sco->active_superblock_offset);
	error = write_exact(sco->fd, inactive_offset, superblock,
	    sizeof(superblock));
	if (error != 0)
		goto out;
	if (fsync(sco->fd) != 0) {
		error = errno;
		goto out;
	}
	sco->sb = next_sb;
	sco->active_superblock_offset = inactive_offset;
out:
	free(root);
	return (error);
}

static int
sco_write_map_entry(struct sco_image *sco, uint64_t cluster_index,
    uint64_t physical_offset)
{
	return (sco_commit_map_entry(sco, cluster_index, SCO_MAP_STATE_PRESENT,
	    physical_offset));
}

static int
sco_write_present_cluster(struct sco_image *sco,
    const struct sco_lookup *lookup, uint64_t cluster_offset,
    const void *buf, size_t len)
{
	uint8_t *cluster;
	uint64_t cluster_index, stored_length;
	int error;

	if (sco == NULL || lookup == NULL || buf == NULL || len == 0)
		return (EINVAL);
	cluster_index = lookup->extent.offset / sco->sb.cluster_size;
	stored_length = sco_cluster_stored_length(sco, cluster_index);
	if (cluster_offset > stored_length || len > stored_length -
	    cluster_offset)
		return (EINVAL);
	if (cluster_offset == 0 && len == stored_length) {
		if (buffer_all_zero(buf, len))
			return (sco_commit_map_entry(sco, cluster_index,
			    SCO_MAP_STATE_ZERO, 0));
		return (write_exact(sco->fd, lookup->physical_offset, buf, len));
	}

	if (stored_length > SIZE_MAX)
		return (EINVAL);
	cluster = malloc((size_t)stored_length);
	if (cluster == NULL)
		return (ENOMEM);
	error = read_exact(sco->fd, lookup->physical_offset, cluster,
	    (size_t)stored_length);
	if (error == 0) {
		memcpy(cluster + cluster_offset, buf, len);
		error = write_exact(sco->fd, lookup->physical_offset, cluster,
		    (size_t)stored_length);
	}
	free(cluster);
	return (error);
}

static int
sco_write_present_cluster_data(struct sco_image *sco,
    const struct sco_lookup *lookup, uint64_t cluster_offset,
    const void *buf, size_t len)
{
	uint8_t *cluster;
	uint64_t cluster_index, stored_length;
	int error;

	if (sco == NULL || lookup == NULL || buf == NULL || len == 0)
		return (EINVAL);
	cluster_index = lookup->extent.offset / sco->sb.cluster_size;
	stored_length = sco_cluster_stored_length(sco, cluster_index);
	if (cluster_offset > stored_length || len > stored_length -
	    cluster_offset)
		return (EINVAL);
	if (cluster_offset == 0 && len == stored_length)
		return (write_exact(sco->fd, lookup->physical_offset, buf, len));

	if (stored_length > SIZE_MAX)
		return (EINVAL);
	cluster = malloc((size_t)stored_length);
	if (cluster == NULL)
		return (ENOMEM);
	error = read_exact(sco->fd, lookup->physical_offset, cluster,
	    (size_t)stored_length);
	if (error == 0) {
		memcpy(cluster + cluster_offset, buf, len);
		error = write_exact(sco->fd, lookup->physical_offset, cluster,
		    (size_t)stored_length);
	}
	free(cluster);
	return (error);
}

static int
sco_write_materialized_cluster(struct sco_image *sco, uint64_t cluster_index,
    uint64_t cluster_offset, const void *buf, size_t len)
{
	uint8_t *cluster;
	uint64_t stored_length, physical_offset;
	int error;

	if (sco == NULL || buf == NULL || len == 0)
		return (EINVAL);
	stored_length = sco_cluster_stored_length(sco, cluster_index);
	if (stored_length == 0 || stored_length > SIZE_MAX ||
	    cluster_offset > stored_length || len > stored_length -
	    cluster_offset)
		return (EINVAL);
	if (cluster_offset == 0 && len == stored_length) {
		if (buffer_all_zero(buf, len))
			return (sco_commit_map_entry(sco, cluster_index,
			    SCO_MAP_STATE_ZERO, 0));
		error = sco_alloc_data_cluster(sco, cluster_index, buf,
		    &physical_offset);
		if (error != 0)
			return (error);
		return (sco_write_map_entry(sco, cluster_index,
		    physical_offset));
	}

	cluster = calloc(1, (size_t)stored_length);
	if (cluster == NULL)
		return (ENOMEM);
	memcpy(cluster + cluster_offset, buf, len);
	error = sco_alloc_data_cluster(sco, cluster_index, cluster,
	    &physical_offset);
	if (error == 0)
		error = sco_write_map_entry(sco, cluster_index,
		    physical_offset);
	free(cluster);
	return (error);
}

static int
sco_write_cluster_range(struct sco_image *sco, uint64_t cluster_index,
    uint64_t cluster_offset, const void *buf, size_t len)
{
	struct sco_lookup lookup;
	uint64_t cluster_start, stored_length;
	int error;

	if (sco == NULL || buf == NULL || len == 0)
		return (EINVAL);
	cluster_start = cluster_index * sco->sb.cluster_size;
	stored_length = sco_cluster_stored_length(sco, cluster_index);
	if (stored_length == 0 || len > stored_length - cluster_offset)
		return (EINVAL);

	memset(&lookup, 0, sizeof(lookup));
	error = sco_lookup_extent(sco, cluster_start, stored_length, &lookup);
	if (error != 0)
		return (error);
	switch (lookup.extent.state) {
	case SCORPI_IMAGE_EXTENT_PRESENT:
		return (sco_write_present_cluster(sco, &lookup,
		    cluster_offset, buf, len));
	case SCORPI_IMAGE_EXTENT_ABSENT:
	case SCORPI_IMAGE_EXTENT_ZERO:
	case SCORPI_IMAGE_EXTENT_DISCARDED:
		return (sco_write_materialized_cluster(sco, cluster_index,
		    cluster_offset, buf, len));
	default:
		return (EINVAL);
	}
}

static int
sco_discard_cluster_range(struct sco_image *sco, uint64_t cluster_index,
    uint64_t cluster_offset, uint64_t len)
{
	struct sco_lookup lookup;
	uint8_t *zeroes;
	uint64_t cluster_start, stored_length;
	int error;

	if (sco == NULL || len == 0 || len > SIZE_MAX)
		return (EINVAL);
	cluster_start = cluster_index * sco->sb.cluster_size;
	stored_length = sco_cluster_stored_length(sco, cluster_index);
	if (stored_length == 0 || cluster_offset >= stored_length ||
	    len > stored_length - cluster_offset)
		return (EINVAL);
	if (cluster_offset == 0 && len == stored_length)
		return (sco_commit_map_entry(sco, cluster_index,
		    SCO_MAP_STATE_DISCARDED, 0));

	memset(&lookup, 0, sizeof(lookup));
	error = sco_lookup_extent(sco, cluster_start, stored_length, &lookup);
	if (error != 0)
		return (error);
	switch (lookup.extent.state) {
	case SCORPI_IMAGE_EXTENT_PRESENT:
		zeroes = calloc(1, (size_t)len);
		if (zeroes == NULL)
			return (ENOMEM);
		error = sco_write_present_cluster(sco, &lookup, cluster_offset,
		    zeroes, (size_t)len);
		free(zeroes);
		return (error);
	case SCORPI_IMAGE_EXTENT_ABSENT:
	case SCORPI_IMAGE_EXTENT_ZERO:
	case SCORPI_IMAGE_EXTENT_DISCARDED:
		return (sco_commit_map_entry(sco, cluster_index,
		    SCO_MAP_STATE_DISCARDED, 0));
	default:
		return (EINVAL);
	}
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
	error = pthread_rwlock_init(&sco->lock, NULL);
	if (error != 0) {
		free(sco);
		return (error);
	}
	error = sco_data_locks_init(sco);
	if (error != 0) {
		pthread_rwlock_destroy(&sco->lock);
		free(sco);
		return (error);
	}
	sco->fd = fd;
	sco->readonly = readonly;
	sco->file_size = file_size;
	error = sco_select_superblock(fd, readonly, file_id_uuid, file_size,
	    &sco->sb, &sco->active_superblock_offset);
	if (error != 0) {
		sco_data_locks_destroy(sco);
		pthread_rwlock_destroy(&sco->lock);
		free(sco);
		return (error);
	}
	if (!readonly && (sco->sb.readonly_compatible_features &
	    SCO_RO_COMPAT_SEALED) != 0) {
		sco_data_locks_destroy(sco);
		pthread_rwlock_destroy(&sco->lock);
		free(sco);
		return (EROFS);
	}
	error = sco_read_base_descriptor(sco);
	if (error != 0) {
		free(sco->base_uri);
		sco_data_locks_destroy(sco);
		pthread_rwlock_destroy(&sco->lock);
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
	int error;

	if (statep == NULL || info == NULL)
		return (EINVAL);
	sco = statep;
	error = pthread_rwlock_rdlock(&sco->lock);
	if (error != 0)
		return (error);
	*info = (struct scorpi_image_info){
		.format = SCORPI_IMAGE_FORMAT_SCO,
		.virtual_size = sco->sb.virtual_size,
		.logical_sector_size = sco->sb.logical_sector_size,
		.physical_sector_size = sco->sb.physical_sector_size,
		.cluster_size = sco->sb.cluster_size,
		.readonly = sco->readonly,
		.sealed = (sco->sb.readonly_compatible_features &
		    SCO_RO_COMPAT_SEALED) != 0,
		.can_discard = !sco->readonly &&
		    (sco->sb.readonly_compatible_features &
		    SCO_RO_COMPAT_SEALED) == 0,
		.has_image_uuid = true,
		.has_image_digest = sco->sb.has_image_digest,
		.has_base = sco->has_base,
		.has_base_uuid = sco->has_base_uuid,
		.has_base_digest = sco->has_base_digest,
	};
	memcpy(info->image_uuid, sco->sb.image_uuid, sizeof(info->image_uuid));
	if (sco->sb.has_image_digest)
		memcpy(info->image_digest, sco->sb.image_digest,
		    sizeof(info->image_digest));
	if (sco->has_base) {
		info->base_uri = strdup(sco->base_uri);
		if (info->base_uri == NULL) {
			pthread_rwlock_unlock(&sco->lock);
			return (ENOMEM);
		}
	}
	if (sco->has_base_uuid)
		memcpy(info->base_uuid, sco->base_uuid,
		    sizeof(info->base_uuid));
	if (sco->has_base_digest)
		memcpy(info->base_digest, sco->base_digest,
		    sizeof(info->base_digest));
	pthread_rwlock_unlock(&sco->lock);
	return (0);
}

static int
sco_map(void *statep, uint64_t offset, uint64_t length,
    struct scorpi_image_extent *extent)
{
	struct sco_image *sco;
	struct sco_lookup lookup;
	int error;

	if (statep == NULL || extent == NULL)
		return (EINVAL);
	sco = statep;
	error = pthread_rwlock_rdlock(&sco->lock);
	if (error != 0)
		return (error);
	memset(&lookup, 0, sizeof(lookup));
	error = sco_lookup_extent(sco, offset, length, &lookup);
	if (error != 0) {
		pthread_rwlock_unlock(&sco->lock);
		return (error);
	}
	*extent = lookup.extent;
	pthread_rwlock_unlock(&sco->lock);
	return (0);
}

static int
sco_validate_write_locked(struct sco_image *sco, uint64_t offset, size_t len)
{
	if (sco == NULL)
		return (EINVAL);
	if (sco->readonly ||
	    (sco->sb.readonly_compatible_features & SCO_RO_COMPAT_SEALED) != 0)
		return (EROFS);
	if (offset > sco->sb.virtual_size ||
	    (uint64_t)len > sco->sb.virtual_size - offset)
		return (EINVAL);
	return (0);
}

static int
sco_read(void *statep, void *buf, uint64_t offset, size_t len)
{
	struct sco_image *sco;
	struct sco_lookup lookup;
	uint8_t *p;
	uint64_t physical_offset;
	pthread_rwlock_t *data_lock;
	size_t n;
	int error;

	if (statep == NULL || (buf == NULL && len != 0))
		return (EINVAL);
	sco = statep;
	error = pthread_rwlock_rdlock(&sco->lock);
	if (error != 0)
		return (error);
	if (offset > sco->sb.virtual_size ||
	    (uint64_t)len > sco->sb.virtual_size - offset) {
		error = EINVAL;
		goto out;
	}

	p = buf;
	while (len > 0) {
		memset(&lookup, 0, sizeof(lookup));
		error = sco_lookup_extent(sco, offset, len, &lookup);
		if (error != 0)
			goto out;
		if (lookup.extent.length == 0 ||
		    lookup.extent.length > SIZE_MAX) {
			error = EIO;
			goto out;
		}
		n = (size_t)lookup.extent.length;
		switch (lookup.extent.state) {
		case SCORPI_IMAGE_EXTENT_PRESENT:
			data_lock = sco_data_lock_for_cluster(sco,
			    offset / sco->sb.cluster_size);
			error = pthread_rwlock_rdlock(data_lock);
			if (error != 0)
				goto out;
			physical_offset = lookup.physical_offset +
			    (offset % sco->sb.cluster_size);
			error = read_exact(sco->fd, physical_offset, p, n);
			pthread_rwlock_unlock(data_lock);
			if (error != 0)
				goto out;
			break;
		case SCORPI_IMAGE_EXTENT_ABSENT:
		case SCORPI_IMAGE_EXTENT_ZERO:
		case SCORPI_IMAGE_EXTENT_DISCARDED:
			memset(p, 0, n);
			break;
		default:
			error = EINVAL;
			goto out;
		}
		offset += n;
		p += n;
		len -= n;
	}
	error = 0;
out:
	pthread_rwlock_unlock(&sco->lock);
	return (error);
}

static bool
sco_write_is_data_only(const struct sco_lookup *lookup,
    uint64_t cluster_offset, const void *buf, size_t len,
    uint64_t stored_length)
{
	if (lookup == NULL || buf == NULL)
		return (false);
	if (lookup->extent.state != SCORPI_IMAGE_EXTENT_PRESENT)
		return (false);
	if (cluster_offset == 0 && len == stored_length &&
	    buffer_all_zero(buf, len))
		return (false);
	return (true);
}

static int
sco_try_write_present_data_only(struct sco_image *sco, uint64_t cluster_index,
    uint64_t cluster_offset, const void *buf, size_t len, bool *donep)
{
	struct sco_lookup lookup;
	pthread_rwlock_t *data_lock;
	uint64_t cluster_start, stored_length;
	int error;

	if (sco == NULL || buf == NULL || len == 0 || donep == NULL)
		return (EINVAL);
	*donep = false;
	error = pthread_rwlock_rdlock(&sco->lock);
	if (error != 0)
		return (error);
	error = sco_validate_write_locked(sco, cluster_index *
	    sco->sb.cluster_size + cluster_offset, len);
	if (error != 0)
		goto out;
	cluster_start = cluster_index * sco->sb.cluster_size;
	stored_length = sco_cluster_stored_length(sco, cluster_index);
	if (stored_length == 0 || cluster_offset >= stored_length ||
	    len > stored_length - cluster_offset) {
		error = EINVAL;
		goto out;
	}
	memset(&lookup, 0, sizeof(lookup));
	error = sco_lookup_extent(sco, cluster_start, stored_length, &lookup);
	if (error != 0)
		goto out;
	if (!sco_write_is_data_only(&lookup, cluster_offset, buf, len,
	    stored_length)) {
		error = 0;
		goto out;
	}

	data_lock = sco_data_lock_for_cluster(sco, cluster_index);
	error = pthread_rwlock_wrlock(data_lock);
	if (error != 0)
		goto out;
	error = sco_write_present_cluster_data(sco, &lookup, cluster_offset,
	    buf, len);
	pthread_rwlock_unlock(data_lock);
	if (error == 0)
		*donep = true;
out:
	pthread_rwlock_unlock(&sco->lock);
	return (error);
}

static int
sco_write_metadata_cluster(struct sco_image *sco, uint64_t cluster_index,
    uint64_t cluster_offset, const void *buf, size_t len)
{
	int error;

	error = pthread_rwlock_wrlock(&sco->lock);
	if (error != 0)
		return (error);
	error = sco_validate_write_locked(sco, cluster_index *
	    sco->sb.cluster_size + cluster_offset, len);
	if (error == 0)
		error = sco_write_cluster_range(sco, cluster_index,
		    cluster_offset, buf, len);
	pthread_rwlock_unlock(&sco->lock);
	return (error);
}

static int
sco_write(void *statep, const void *buf, uint64_t offset, size_t len)
{
	struct sco_image *sco;
	const uint8_t *p;
	uint64_t end, cluster_index, cluster_offset, stored_length, chunk;
	uint64_t cluster_size, virtual_size;
	bool done;
	int error;

	if (statep == NULL || (buf == NULL && len != 0))
		return (EINVAL);
	sco = statep;
	error = pthread_rwlock_rdlock(&sco->lock);
	if (error != 0)
		return (error);
	error = sco_validate_write_locked(sco, offset, len);
	cluster_size = sco->sb.cluster_size;
	virtual_size = sco->sb.virtual_size;
	pthread_rwlock_unlock(&sco->lock);
	if (error != 0 || len == 0)
		return (error);

	p = buf;
	end = offset + (uint64_t)len;
	while (offset < end) {
		cluster_index = offset / cluster_size;
		cluster_offset = offset % cluster_size;
		stored_length = virtual_size - cluster_index * cluster_size;
		if (stored_length > cluster_size)
			stored_length = cluster_size;
		if (stored_length == 0 || cluster_offset >= stored_length) {
			error = EINVAL;
			return (error);
		}
		chunk = stored_length - cluster_offset;
		if (chunk > end - offset)
			chunk = end - offset;
		if (chunk > SIZE_MAX) {
			error = EINVAL;
			return (error);
		}

		done = false;
		error = sco_try_write_present_data_only(sco, cluster_index,
		    cluster_offset, p, (size_t)chunk, &done);
		if (error != 0)
			return (error);
		if (!done) {
			error = sco_write_metadata_cluster(sco, cluster_index,
			    cluster_offset, p, (size_t)chunk);
			if (error != 0)
				return (error);
		}

		offset += chunk;
		p += chunk;
	}
	return (0);
}

static int
sco_discard(void *statep,
    uint64_t offset, uint64_t length)
{
	struct sco_image *sco;
	uint64_t end, cluster_index, cluster_offset, stored_length, chunk;
	int error;

	if (statep == NULL)
		return (EINVAL);
	sco = statep;
	error = pthread_rwlock_wrlock(&sco->lock);
	if (error != 0)
		return (error);
	if (sco->readonly ||
	    (sco->sb.readonly_compatible_features & SCO_RO_COMPAT_SEALED) != 0) {
		error = EROFS;
		goto out;
	}
	if (offset > sco->sb.virtual_size ||
	    length > sco->sb.virtual_size - offset) {
		error = EINVAL;
		goto out;
	}
	if (length == 0) {
		error = 0;
		goto out;
	}

	end = offset + length;
	while (offset < end) {
		cluster_index = offset / sco->sb.cluster_size;
		cluster_offset = offset % sco->sb.cluster_size;
		stored_length = sco_cluster_stored_length(sco, cluster_index);
		if (stored_length == 0 || cluster_offset >= stored_length) {
			error = EINVAL;
			goto out;
		}
		chunk = stored_length - cluster_offset;
		if (chunk > end - offset)
			chunk = end - offset;
		error = sco_discard_cluster_range(sco, cluster_index,
		    cluster_offset, chunk);
		if (error != 0)
			goto out;
		offset += chunk;
	}
	error = 0;
out:
	pthread_rwlock_unlock(&sco->lock);
	return (error);
}

static int
sco_flush(void *statep)
{
	struct sco_image *sco;
	int error;

	if (statep == NULL)
		return (EINVAL);
	sco = statep;
	error = pthread_rwlock_wrlock(&sco->lock);
	if (error != 0)
		return (error);
	if (fsync(sco->fd) != 0)
		error = errno;
	else
		error = 0;
	pthread_rwlock_unlock(&sco->lock);
	return (error);
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
	error = pthread_rwlock_wrlock(&sco->lock);
	if (error != 0)
		return (error);
	error = 0;
	if (sco->fd >= 0 && close(sco->fd) != 0)
		error = errno;
	free(sco->base_uri);
	pthread_rwlock_unlock(&sco->lock);
	sco_data_locks_destroy(sco);
	pthread_rwlock_destroy(&sco->lock);
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
SCORPI_IMAGE_BACKEND_SET(scorpi_sco_ops);
