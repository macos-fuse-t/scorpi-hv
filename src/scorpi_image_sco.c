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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <support/endian.h>

#include "scorpi_crc32c.h"
#include "scorpi_host_sparse.h"
#include "scorpi_image.h"
#include "scorpi_image_sco.h"
#include "scorpi_image_sco_format.h"

#define	SCO_DATA_LOCK_STRIPES		64
#define	SCO_SLOT_A			0
#define	SCO_SLOT_B			1

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
	uint64_t table_offset;
	uint32_t table_length;
	uint32_t incompatible_features;
	uint32_t readonly_compatible_features;
	uint32_t compatible_features;
	uint8_t image_uuid[16];
	uint8_t image_digest[32];
	bool has_image_digest;
};

struct sco_table_entry {
	uint32_t generation;
	uint32_t physical_cluster;
	uint8_t state;
	uint8_t active_slot;
	uint8_t flags;
	uint8_t reserved;
};

struct sco_image {
	int fd;
	pthread_rwlock_t lock;
	pthread_rwlock_t data_locks[SCO_DATA_LOCK_STRIPES];
	bool readonly;
	bool trace;
	uint64_t file_size;
	uint64_t active_superblock_offset;
	struct sco_superblock_decoded sb;
	struct sco_table_entry *table;
	uint8_t *data_cluster_used;
	uint64_t data_cluster_base;
	uint64_t data_cluster_count;
	uint64_t data_alloc_cursor;
	bool has_base;
	char *base_uri;
	uint8_t base_uuid[16];
	bool has_base_uuid;
	uint8_t base_digest[32];
	bool has_base_digest;
	uint64_t trace_read_calls;
	uint64_t trace_read_bytes;
	uint64_t trace_write_calls;
	uint64_t trace_write_bytes;
	uint64_t trace_data_only_writes;
	uint64_t trace_metadata_writes;
	uint64_t trace_materialize_writes;
	uint64_t trace_alloc_data_calls;
	uint64_t trace_alloc_data_bytes;
	uint64_t trace_commit_table_calls;
	uint64_t trace_commit_table_ns;
	uint64_t trace_fsync_calls;
	uint64_t trace_fsync_ns;
	uint64_t trace_flush_calls;
};

struct sco_lookup {
	struct scorpi_image_extent extent;
	uint64_t physical_offset;
};

static uint64_t
sco_trace_now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return (0);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

static uint64_t
sco_trace_add(uint64_t *counter, uint64_t value)
{
	return (__atomic_add_fetch(counter, value, __ATOMIC_RELAXED));
}

static uint64_t
sco_trace_load(const uint64_t *counter)
{
	return (__atomic_load_n(counter, __ATOMIC_RELAXED));
}

static void
sco_trace_report(struct sco_image *sco, const char *where)
{
	if (sco == NULL || !sco->trace)
		return;
	fprintf(stderr,
	    "SCO_TRACE[%s]: reads=%llu read_bytes=%llu writes=%llu "
	    "write_bytes=%llu data_only=%llu metadata_writes=%llu "
	    "materialize=%llu alloc_data=%llu alloc_data_bytes=%llu "
	    "commit_table=%llu commit_table_ms=%llu fsyncs=%llu "
	    "fsync_ms=%llu flushes=%llu\n",
	    where,
	    (unsigned long long)sco_trace_load(&sco->trace_read_calls),
	    (unsigned long long)sco_trace_load(&sco->trace_read_bytes),
	    (unsigned long long)sco_trace_load(&sco->trace_write_calls),
	    (unsigned long long)sco_trace_load(&sco->trace_write_bytes),
	    (unsigned long long)sco_trace_load(&sco->trace_data_only_writes),
	    (unsigned long long)sco_trace_load(&sco->trace_metadata_writes),
	    (unsigned long long)sco_trace_load(&sco->trace_materialize_writes),
	    (unsigned long long)sco_trace_load(&sco->trace_alloc_data_calls),
	    (unsigned long long)sco_trace_load(&sco->trace_alloc_data_bytes),
	    (unsigned long long)sco_trace_load(&sco->trace_commit_table_calls),
	    (unsigned long long)(sco_trace_load(&sco->trace_commit_table_ns) /
	    1000000ULL),
	    (unsigned long long)sco_trace_load(&sco->trace_fsync_calls),
	    (unsigned long long)(sco_trace_load(&sco->trace_fsync_ns) /
	    1000000ULL),
	    (unsigned long long)sco_trace_load(&sco->trace_flush_calls));
}

static int
sco_trace_fsync(struct sco_image *sco)
{
	uint64_t start, end;

	start = sco_trace_now_ns();
	if (fsync(sco->fd) != 0)
		return (errno);
	end = sco_trace_now_ns();
	if (sco->trace) {
		sco_trace_add(&sco->trace_fsync_calls, 1);
		if (end >= start)
			sco_trace_add(&sco->trace_fsync_ns, end - start);
	}
	return (0);
}

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

	if (len == 0)
		return (true);
	p = buf;
	return (p[0] == 0 && (len == 1 || memcmp(p, p + 1, len - 1) == 0));
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
	actual = scorpi_crc32c(buf, len);
	memcpy(buf + checksum_offset, saved, sizeof(saved));
	return (actual == expected);
}

static uint64_t
sco_inactive_superblock_offset(uint64_t active_offset)
{
	return (active_offset == SCO_SUPERBLOCK_A_OFFSET ?
	    SCO_SUPERBLOCK_B_OFFSET : SCO_SUPERBLOCK_A_OFFSET);
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

static bool
sco_state_valid(uint8_t state)
{
	return (state == SCO_MAP_STATE_ABSENT ||
	    state == SCO_MAP_STATE_PRESENT ||
	    state == SCO_MAP_STATE_ZERO ||
	    state == SCO_MAP_STATE_DISCARDED);
}

static uint64_t
sco_table_slot_offset(const struct sco_image *sco, uint64_t cluster_index,
    uint8_t slot_index)
{
	return (sco->sb.table_offset + cluster_index * SCO_TABLE_ENTRY_SIZE +
	    slot_index * SCO_TABLE_SLOT_SIZE);
}

static uint32_t
sco_slot_crc32c(const struct sco_image *sco, uint64_t cluster_index,
    uint8_t slot_index, const uint8_t slot[SCO_TABLE_SLOT_SIZE])
{
	uint8_t context[16 + 8 + 4 + SCO_TABLE_SLOT_SIZE];
	uint8_t slot_copy[SCO_TABLE_SLOT_SIZE];
	uint32_t slot_index32;

	memcpy(slot_copy, slot, sizeof(slot_copy));
	memset(slot_copy + 0x0008, 0, 4);
	memcpy(context, sco->sb.image_uuid, sizeof(sco->sb.image_uuid));
	le64enc(context + 16, cluster_index);
	slot_index32 = slot_index;
	le32enc(context + 24, slot_index32);
	memcpy(context + 28, slot_copy, sizeof(slot_copy));
	return (scorpi_crc32c(context, sizeof(context)));
}

static void
sco_build_slot(struct sco_image *sco, uint64_t cluster_index,
    uint8_t slot_index, uint32_t generation, uint8_t state,
    uint32_t physical_cluster, uint8_t slot[SCO_TABLE_SLOT_SIZE])
{
	uint32_t crc;

	memset(slot, 0, SCO_TABLE_SLOT_SIZE);
	le32enc(slot + 0x0000, generation);
	le32enc(slot + 0x0004, physical_cluster);
	slot[0x000c] = state;
	slot[0x000d] = 0;
	le16enc(slot + 0x000e, 0);
	crc = sco_slot_crc32c(sco, cluster_index, slot_index, slot);
	le32enc(slot + 0x0008, crc);
}

static bool
sco_slot_valid(struct sco_image *sco, uint64_t cluster_index,
    uint8_t slot_index, const uint8_t slot[SCO_TABLE_SLOT_SIZE],
    struct sco_table_entry *entry)
{
	uint32_t generation, physical_cluster, expected_crc;
	uint64_t physical_offset;
	uint8_t state, flags;

	generation = le32dec(slot + 0x0000);
	physical_cluster = le32dec(slot + 0x0004);
	expected_crc = le32dec(slot + 0x0008);
	state = slot[0x000c];
	flags = slot[0x000d];
	if (generation == 0 && physical_cluster == 0 && expected_crc == 0 &&
	    state == 0 && flags == 0 && le16dec(slot + 0x000e) == 0)
		return (false);
	if (flags != 0 || le16dec(slot + 0x000e) != 0 ||
	    !sco_state_valid(state))
		return (false);
	if (sco_slot_crc32c(sco, cluster_index, slot_index, slot) !=
	    expected_crc)
		return (false);
	if (state == SCO_MAP_STATE_PRESENT) {
		if (physical_cluster == 0)
			return (false);
		if (physical_cluster >
		    UINT64_MAX / (uint64_t)sco->sb.cluster_size)
			return (false);
		physical_offset = (uint64_t)physical_cluster *
		    sco->sb.cluster_size;
		if (physical_offset < sco->sb.data_area_offset ||
		    physical_offset > sco->file_size ||
		    sco_cluster_stored_length(sco, cluster_index) >
		    sco->file_size - physical_offset)
			return (false);
	} else if (physical_cluster != 0) {
		return (false);
	}
	entry->generation = generation;
	entry->physical_cluster = physical_cluster;
	entry->state = state;
	entry->active_slot = slot_index;
	entry->flags = flags;
	entry->reserved = 0;
	return (true);
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
	le64enc(buf + 0x0060, sb->table_offset);
	le32enc(buf + 0x0068, sb->table_length);
	le32enc(buf + 0x006c, SCO_TABLE_SLOT_SIZE);
	le32enc(buf + 0x0070, sb->incompatible_features);
	le32enc(buf + 0x0074, sb->readonly_compatible_features);
	le32enc(buf + 0x0078, sb->compatible_features);
	memcpy(buf + 0x0080, sb->image_uuid, sizeof(sb->image_uuid));
	if (sb->has_image_digest)
		memcpy(buf + 0x0090, sb->image_digest,
		    sizeof(sb->image_digest));
	le32enc(buf + 0x00b0, sb->has_image_digest ? 1 : 0);
	crc = scorpi_crc32c(buf, SCO_SUPERBLOCK_SIZE);
	le32enc(buf + 0x0014, crc);
}

static int
sco_commit_superblock(struct sco_image *sco,
    const struct sco_superblock_decoded *next_sbp)
{
	uint8_t superblock[SCO_SUPERBLOCK_SIZE];
	uint64_t inactive_offset;
	int error;

	if (sco == NULL || next_sbp == NULL)
		return (EINVAL);
	sco_build_superblock(superblock, next_sbp);
	inactive_offset =
	    sco_inactive_superblock_offset(sco->active_superblock_offset);
	error = write_exact(sco->fd, inactive_offset, superblock,
	    sizeof(superblock));
	if (error != 0)
		return (error);
	if (fsync(sco->fd) != 0)
		return (errno);
	sco->sb = *next_sbp;
	sco->active_superblock_offset = inactive_offset;
	return (0);
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
	uint64_t expected_clusters, required_table_length, table_total_length;

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
	if (sb->cluster_count > UINT64_MAX / SCO_TABLE_SLOT_SIZE)
		return (false);
	required_table_length = sb->cluster_count * SCO_TABLE_SLOT_SIZE;
	if (required_table_length > SCO_MAX_SINGLE_TABLE_BYTES ||
	    required_table_length > UINT32_MAX)
		return (false);
	if (sb->table_length < required_table_length ||
	    sb->table_length > SCO_MAX_SINGLE_TABLE_BYTES ||
	    (sb->table_length % SCO_METADATA_PAGE_SIZE) != 0)
		return (false);
	table_total_length = sb->table_length * SCO_TABLE_SLOTS_PER_ENTRY;
	if (!range_before_data(sb->table_offset, table_total_length,
	    sb->data_area_offset))
		return (false);
	if (sb->data_area_offset < SCO_METADATA_AREA_OFFSET ||
	    (sb->data_area_offset % sb->cluster_size) != 0 ||
	    sb->data_area_offset > file_size)
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
	uint32_t table_slot_size, has_image_digest;
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
	table_slot_size = le32dec(buf + 0x006c);
	if (table_slot_size != SCO_TABLE_SLOT_SIZE)
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
	sb.table_offset = le64dec(buf + 0x0060);
	sb.table_length = le32dec(buf + 0x0068);
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
	if ((sb.incompatible_features & SCO_INCOMPAT_FIXED_TABLE_V2) == 0)
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

static int
sco_load_table(struct sco_image *sco)
{
	struct sco_table_entry a, b;
	uint8_t *slots;
	uint64_t i, bytes;
	size_t table_entries;
	bool valid_a, valid_b;
	int error;

	if (sco == NULL || sco->sb.cluster_count > SIZE_MAX /
	    sizeof(*sco->table))
		return (EINVAL);
	table_entries = (size_t)sco->sb.cluster_count;
	sco->table = calloc(table_entries, sizeof(*sco->table));
	if (sco->table == NULL)
		return (ENOMEM);
	if (sco->sb.cluster_count > UINT64_MAX / SCO_TABLE_ENTRY_SIZE)
		return (EINVAL);
	bytes = sco->sb.cluster_count * SCO_TABLE_ENTRY_SIZE;
	if (bytes > SIZE_MAX)
		return (EINVAL);
	slots = malloc((size_t)bytes);
	if (slots == NULL)
		return (ENOMEM);
	error = read_exact(sco->fd, sco->sb.table_offset, slots,
	    (size_t)bytes);
	if (error != 0) {
		free(slots);
		return (error);
	}
	for (i = 0; i < sco->sb.cluster_count; i++) {
		valid_a = sco_slot_valid(sco, i, SCO_SLOT_A,
		    slots + i * SCO_TABLE_ENTRY_SIZE, &a);
		valid_b = sco_slot_valid(sco, i, SCO_SLOT_B,
		    slots + i * SCO_TABLE_ENTRY_SIZE + SCO_TABLE_SLOT_SIZE,
		    &b);
		if (valid_a && valid_b)
			sco->table[i] = b.generation > a.generation ? b : a;
		else if (valid_a)
			sco->table[i] = a;
		else if (valid_b)
			sco->table[i] = b;
		else
			sco->table[i] = (struct sco_table_entry){
				.state = SCO_MAP_STATE_ABSENT,
				.active_slot = SCO_SLOT_B,
			};
	}
	free(slots);
	return (0);
}

static int
sco_data_allocator_grow(struct sco_image *sco, uint64_t count)
{
	uint8_t *used;

	if (sco == NULL)
		return (EINVAL);
	if (count <= sco->data_cluster_count)
		return (0);
	if (count > SIZE_MAX)
		return (EFBIG);
	used = realloc(sco->data_cluster_used, (size_t)count);
	if (used == NULL)
		return (ENOMEM);
	memset(used + sco->data_cluster_count, 0,
	    (size_t)(count - sco->data_cluster_count));
	sco->data_cluster_used = used;
	sco->data_cluster_count = count;
	return (0);
}

static int
sco_data_allocator_mark(struct sco_image *sco, uint32_t physical_cluster,
    bool used)
{
	uint64_t index;

	if (sco == NULL || physical_cluster < sco->data_cluster_base)
		return (EINVAL);
	index = (uint64_t)physical_cluster - sco->data_cluster_base;
	if (index >= sco->data_cluster_count)
		return (EINVAL);
	sco->data_cluster_used[index] = used ? 1 : 0;
	return (0);
}

static void
sco_data_allocator_release(struct sco_image *sco, uint32_t physical_cluster)
{
	if (sco_data_allocator_mark(sco, physical_cluster, false) != 0)
		return;
	if (sco->data_alloc_cursor >= sco->data_cluster_count ||
	    (uint64_t)physical_cluster - sco->data_cluster_base <
	    sco->data_alloc_cursor)
		sco->data_alloc_cursor =
		    (uint64_t)physical_cluster - sco->data_cluster_base;
}

static int
sco_data_allocator_init(struct sco_image *sco)
{
	const struct sco_table_entry *entry;
	uint64_t i, count, index, file_clusters;
	int error;

	if (sco == NULL || sco->table == NULL ||
	    (sco->sb.data_area_offset % sco->sb.cluster_size) != 0)
		return (EINVAL);
	sco->data_cluster_base = sco->sb.data_area_offset /
	    sco->sb.cluster_size;
	if (sco->file_size <= sco->sb.data_area_offset) {
		count = 0;
	} else {
		if (sco->file_size > UINT64_MAX - sco->sb.cluster_size + 1)
			return (EFBIG);
		file_clusters = (sco->file_size + sco->sb.cluster_size - 1) /
		    sco->sb.cluster_size;
		if (file_clusters < sco->data_cluster_base)
			return (EINVAL);
		count = file_clusters - sco->data_cluster_base;
	}
	error = sco_data_allocator_grow(sco, count);
	if (error != 0)
		return (error);
	for (i = 0; i < sco->sb.cluster_count; i++) {
		entry = &sco->table[i];
		if (entry->state != SCO_MAP_STATE_PRESENT)
			continue;
		if (entry->physical_cluster < sco->data_cluster_base)
			return (EINVAL);
		index = (uint64_t)entry->physical_cluster -
		    sco->data_cluster_base;
		if (index >= sco->data_cluster_count ||
		    sco->data_cluster_used[index] != 0)
			return (EINVAL);
		sco->data_cluster_used[index] = 1;
	}
	return (0);
}

static int
sco_lookup_extent(struct sco_image *sco, uint64_t offset, uint64_t length,
    struct sco_lookup *lookup)
{
	const struct sco_table_entry *entry;
	uint64_t cluster_index, cluster_start, cluster_offset, cluster_length;
	uint64_t physical_offset;

	if (sco == NULL || lookup == NULL || length == 0 ||
	    offset >= sco->sb.virtual_size)
		return (EINVAL);
	cluster_index = offset / sco->sb.cluster_size;
	if (cluster_index >= sco->sb.cluster_count)
		return (EINVAL);
	cluster_start = cluster_index * sco->sb.cluster_size;
	cluster_offset = offset - cluster_start;
	cluster_length = sco_cluster_stored_length(sco, cluster_index);
	if (cluster_offset >= cluster_length)
		return (EINVAL);
	if (length > cluster_length - cluster_offset)
		length = cluster_length - cluster_offset;

	entry = &sco->table[cluster_index];
	lookup->extent = (struct scorpi_image_extent){
		.offset = offset,
		.length = length,
	};
	lookup->physical_offset = 0;
	switch (entry->state) {
	case SCO_MAP_STATE_PRESENT:
		physical_offset = (uint64_t)entry->physical_cluster *
		    sco->sb.cluster_size;
		lookup->extent.state = SCORPI_IMAGE_EXTENT_PRESENT;
		lookup->physical_offset = physical_offset;
		break;
	case SCO_MAP_STATE_ZERO:
		lookup->extent.state = SCORPI_IMAGE_EXTENT_ZERO;
		break;
	case SCO_MAP_STATE_DISCARDED:
		lookup->extent.state = SCORPI_IMAGE_EXTENT_DISCARDED;
		break;
	case SCO_MAP_STATE_ABSENT:
		lookup->extent.state = SCORPI_IMAGE_EXTENT_ABSENT;
		break;
	default:
		return (EINVAL);
	}
	return (0);
}

static int
sco_alloc_data_cluster(struct sco_image *sco, uint64_t cluster_index,
    const void *buf, uint64_t *physical_offsetp)
{
	uint64_t checked, index, physical_offset, stored_length, end;
	uint64_t physical_cluster;
	int error;

	if (sco == NULL || buf == NULL || physical_offsetp == NULL)
		return (EINVAL);
	stored_length = sco_cluster_stored_length(sco, cluster_index);
	if (stored_length == 0 || stored_length > SIZE_MAX)
		return (EINVAL);
	physical_cluster = 0;
	for (checked = 0; checked < sco->data_cluster_count; checked++) {
		index = (sco->data_alloc_cursor + checked) %
		    sco->data_cluster_count;
		if (sco->data_cluster_used[index] == 0) {
			physical_cluster = sco->data_cluster_base + index;
			sco->data_alloc_cursor = (index + 1) %
			    sco->data_cluster_count;
			break;
		}
	}
	if (physical_cluster == 0) {
		if (sco->data_cluster_base > UINT64_MAX -
		    sco->data_cluster_count)
			return (EFBIG);
		physical_cluster = sco->data_cluster_base +
		    sco->data_cluster_count;
		if (physical_cluster == 0 || physical_cluster > UINT32_MAX)
			return (EFBIG);
		error = sco_data_allocator_grow(sco,
		    sco->data_cluster_count + 1);
		if (error != 0)
			return (error);
		sco->data_alloc_cursor = sco->data_cluster_count == 0 ? 0 :
		    (physical_cluster - sco->data_cluster_base + 1) %
		    sco->data_cluster_count;
	}
	if (physical_cluster == 0 || physical_cluster > UINT32_MAX)
		return (EFBIG);
	sco->data_cluster_used[physical_cluster - sco->data_cluster_base] = 1;
	physical_offset = physical_cluster * (uint64_t)sco->sb.cluster_size;
	if (stored_length > UINT64_MAX - physical_offset)
		return (EFBIG);
	end = physical_offset + stored_length;
	error = write_exact(sco->fd, physical_offset, buf,
	    (size_t)stored_length);
	if (error != 0)
		return (error);
	if (end > sco->file_size)
		sco->file_size = end;
	if (sco->trace) {
		sco_trace_add(&sco->trace_alloc_data_calls, 1);
		sco_trace_add(&sco->trace_alloc_data_bytes, stored_length);
	}
	*physical_offsetp = physical_offset;
	return (0);
}

static int
sco_commit_table_entry(struct sco_image *sco, uint64_t cluster_index,
    uint8_t state, uint64_t physical_offset)
{
	struct sco_table_entry *entry;
	struct sco_table_entry old_entry;
	uint8_t slot[SCO_TABLE_SLOT_SIZE], next_slot;
	uint64_t old_physical_offset, trace_start, trace_end, physical_cluster;
	uint32_t next_generation;
	int error;

	if (sco == NULL || cluster_index >= sco->sb.cluster_count ||
	    !sco_state_valid(state))
		return (EINVAL);
	if (state == SCO_MAP_STATE_PRESENT) {
		if (physical_offset < sco->sb.data_area_offset ||
		    (physical_offset % sco->sb.cluster_size) != 0)
			return (EINVAL);
		physical_cluster = physical_offset / sco->sb.cluster_size;
		if (physical_cluster == 0 || physical_cluster > UINT32_MAX)
			return (EFBIG);
	} else {
		if (physical_offset != 0)
			return (EINVAL);
		physical_cluster = 0;
	}

	trace_start = sco_trace_now_ns();
	if (sco->trace)
		sco_trace_add(&sco->trace_commit_table_calls, 1);
	entry = &sco->table[cluster_index];
	old_entry = *entry;
	if (entry->generation == UINT32_MAX)
		return (EFBIG);
	next_generation = entry->generation + 1;
	next_slot = entry->active_slot == SCO_SLOT_A ? SCO_SLOT_B : SCO_SLOT_A;
	sco_build_slot(sco, cluster_index, next_slot, next_generation, state,
	    (uint32_t)physical_cluster, slot);
	error = write_exact(sco->fd,
	    sco_table_slot_offset(sco, cluster_index, next_slot), slot,
	    sizeof(slot));
	if (error != 0)
		return (error);
	error = sco_trace_fsync(sco);
	if (error != 0)
		return (error);
	*entry = (struct sco_table_entry){
		.generation = next_generation,
		.physical_cluster = (uint32_t)physical_cluster,
		.state = state,
		.active_slot = next_slot,
	};
	if (old_entry.state == SCO_MAP_STATE_PRESENT &&
	    old_entry.physical_cluster != physical_cluster) {
		sco_data_allocator_release(sco, old_entry.physical_cluster);
		old_physical_offset = (uint64_t)old_entry.physical_cluster *
		    sco->sb.cluster_size;
		(void)scorpi_host_punch_hole(sco->fd, old_physical_offset,
		    sco_cluster_stored_length(sco, cluster_index));
	}
	trace_end = sco_trace_now_ns();
	if (sco->trace && trace_end >= trace_start)
		sco_trace_add(&sco->trace_commit_table_ns,
		    trace_end - trace_start);
	return (0);
}

static int
sco_write_present_cluster(struct sco_image *sco,
    const struct sco_lookup *lookup, uint64_t cluster_offset,
    const void *buf, size_t len)
{
	uint64_t cluster_index, stored_length;

	if (sco == NULL || lookup == NULL || buf == NULL || len == 0)
		return (EINVAL);
	cluster_index = lookup->extent.offset / sco->sb.cluster_size;
	stored_length = sco_cluster_stored_length(sco, cluster_index);
	if (cluster_offset > stored_length || len > stored_length -
	    cluster_offset)
		return (EINVAL);
	if (cluster_offset == 0 && len == stored_length) {
		if (buffer_all_zero(buf, len)) {
			return (sco_commit_table_entry(sco, cluster_index,
			    SCO_MAP_STATE_ZERO, 0));
		}
	}
	return (write_exact(sco->fd, lookup->physical_offset + cluster_offset,
	    buf, len));
}

static int
sco_write_present_cluster_data(struct sco_image *sco,
    const struct sco_lookup *lookup, uint64_t cluster_offset,
    const void *buf, size_t len)
{
	uint64_t cluster_index, stored_length;

	if (sco == NULL || lookup == NULL || buf == NULL || len == 0)
		return (EINVAL);
	cluster_index = lookup->extent.offset / sco->sb.cluster_size;
	stored_length = sco_cluster_stored_length(sco, cluster_index);
	if (cluster_offset > stored_length || len > stored_length -
	    cluster_offset)
		return (EINVAL);
	return (write_exact(sco->fd, lookup->physical_offset + cluster_offset,
	    buf, len));
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
	if (sco->trace)
		sco_trace_add(&sco->trace_materialize_writes, 1);
	stored_length = sco_cluster_stored_length(sco, cluster_index);
	if (stored_length == 0 || stored_length > SIZE_MAX ||
	    cluster_offset > stored_length || len > stored_length -
	    cluster_offset)
		return (EINVAL);
	if (cluster_offset == 0 && len == stored_length) {
		if (buffer_all_zero(buf, len))
			return (sco_commit_table_entry(sco, cluster_index,
			    SCO_MAP_STATE_ZERO, 0));
		error = sco_alloc_data_cluster(sco, cluster_index, buf,
		    &physical_offset);
		if (error != 0)
			return (error);
		error = sco_trace_fsync(sco);
		if (error != 0)
			return (error);
		return (sco_commit_table_entry(sco, cluster_index,
		    SCO_MAP_STATE_PRESENT, physical_offset));
	}

	cluster = calloc(1, (size_t)stored_length);
	if (cluster == NULL)
		return (ENOMEM);
	memcpy(cluster + cluster_offset, buf, len);
	error = sco_alloc_data_cluster(sco, cluster_index, cluster,
	    &physical_offset);
	if (error == 0)
		error = sco_trace_fsync(sco);
	if (error == 0)
		error = sco_commit_table_entry(sco, cluster_index,
		    SCO_MAP_STATE_PRESENT, physical_offset);
	free(cluster);
	return (error);
}

static int
sco_write_cluster_range(struct sco_image *sco, uint64_t cluster_index,
    uint64_t cluster_offset, const void *buf, size_t len)
{
	struct sco_lookup lookup;
	uint64_t cluster_start, stored_length;
	pthread_rwlock_t *data_lock;
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
	data_lock = sco_data_lock_for_cluster(sco, cluster_index);
	error = pthread_rwlock_wrlock(data_lock);
	if (error != 0)
		return (error);
	switch (lookup.extent.state) {
	case SCORPI_IMAGE_EXTENT_PRESENT:
		error = sco_write_present_cluster(sco, &lookup,
		    cluster_offset, buf, len);
		break;
	case SCORPI_IMAGE_EXTENT_ABSENT:
		if (sco->has_base && (cluster_offset != 0 ||
		    len != stored_length)) {
			error = EOPNOTSUPP;
			break;
		}
		/* FALLTHROUGH */
	case SCORPI_IMAGE_EXTENT_ZERO:
	case SCORPI_IMAGE_EXTENT_DISCARDED:
		error = sco_write_materialized_cluster(sco, cluster_index,
		    cluster_offset, buf, len);
		break;
	default:
		error = EINVAL;
		break;
	}
	pthread_rwlock_unlock(data_lock);
	return (error);
}

static int
sco_discard_cluster_range(struct sco_image *sco, uint64_t cluster_index,
    uint64_t cluster_offset, uint64_t len)
{
	struct sco_lookup lookup;
	uint8_t *zeroes;
	uint64_t cluster_start, stored_length;
	pthread_rwlock_t *data_lock;
	int error;

	if (sco == NULL || len == 0 || len > SIZE_MAX)
		return (EINVAL);
	cluster_start = cluster_index * sco->sb.cluster_size;
	stored_length = sco_cluster_stored_length(sco, cluster_index);
	if (stored_length == 0 || cluster_offset >= stored_length ||
	    len > stored_length - cluster_offset)
		return (EINVAL);

	memset(&lookup, 0, sizeof(lookup));
	error = sco_lookup_extent(sco, cluster_start, stored_length, &lookup);
	if (error != 0)
		return (error);
	data_lock = sco_data_lock_for_cluster(sco, cluster_index);
	error = pthread_rwlock_wrlock(data_lock);
	if (error != 0)
		return (error);
	if (cluster_offset == 0 && len == stored_length) {
		error = sco_commit_table_entry(sco, cluster_index,
		    SCO_MAP_STATE_DISCARDED, 0);
		pthread_rwlock_unlock(data_lock);
		return (error);
	}
	switch (lookup.extent.state) {
	case SCORPI_IMAGE_EXTENT_PRESENT:
		zeroes = calloc(1, (size_t)len);
		if (zeroes == NULL) {
			error = ENOMEM;
			break;
		}
		error = sco_write_present_cluster(sco, &lookup, cluster_offset,
		    zeroes, (size_t)len);
		free(zeroes);
		break;
	case SCORPI_IMAGE_EXTENT_ABSENT:
		if (sco->has_base) {
			error = EOPNOTSUPP;
			break;
		}
		/* FALLTHROUGH */
	case SCORPI_IMAGE_EXTENT_ZERO:
	case SCORPI_IMAGE_EXTENT_DISCARDED:
			error = sco_commit_table_entry(sco, cluster_index,
			    SCO_MAP_STATE_DISCARDED, 0);
		break;
	default:
		error = EINVAL;
		break;
	}
	pthread_rwlock_unlock(data_lock);
	return (error);
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
	*statep = NULL;
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
	sco->trace = getenv("SCORPI_SCO_TRACE") != NULL;
	sco->file_size = file_size;
	error = sco_select_superblock(fd, readonly, file_id_uuid, file_size,
	    &sco->sb, &sco->active_superblock_offset);
	if (error != 0)
		goto fail;
	if (!readonly && (sco->sb.readonly_compatible_features &
	    SCO_RO_COMPAT_SEALED) != 0) {
		error = EROFS;
		goto fail;
	}
	error = sco_load_table(sco);
	if (error != 0)
		goto fail;
	error = sco_data_allocator_init(sco);
	if (error != 0)
		goto fail;
	error = sco_read_base_descriptor(sco);
	if (error != 0)
		goto fail;
	*statep = sco;
	return (0);

fail:
	free(sco->base_uri);
	free(sco->data_cluster_used);
	free(sco->table);
	sco_data_locks_destroy(sco);
	pthread_rwlock_destroy(&sco->lock);
	free(sco);
	return (error);
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
	if (error == 0)
		*extent = lookup.extent;
	pthread_rwlock_unlock(&sco->lock);
	return (error);
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
	if (sco->trace) {
		sco_trace_add(&sco->trace_read_calls, 1);
		sco_trace_add(&sco->trace_read_bytes, len);
	}
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
	if (sco->trace)
		sco_trace_add(&sco->trace_data_only_writes, 1);
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
	if (sco->trace)
		sco_trace_add(&sco->trace_metadata_writes, 1);
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
	if (sco->trace) {
		uint64_t calls;

		calls = sco_trace_add(&sco->trace_write_calls, 1);
		sco_trace_add(&sco->trace_write_bytes, len);
		if ((calls % 1000) == 0)
			sco_trace_report(sco, "write");
	}
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
		if (stored_length == 0 || cluster_offset >= stored_length)
			return (EINVAL);
		chunk = stored_length - cluster_offset;
		if (chunk > end - offset)
			chunk = end - offset;
		if (chunk > SIZE_MAX)
			return (EINVAL);

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
sco_discard(void *statep, uint64_t offset, uint64_t length)
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
	sco_trace_add(&sco->trace_flush_calls, 1);
	error = sco_trace_fsync(sco);
	pthread_rwlock_unlock(&sco->lock);
	return (error);
}

int
scorpi_image_sco_set_sealed(void *statep, bool sealed)
{
	struct sco_superblock_decoded next_sb;
	struct sco_image *sco;
	bool currently_sealed;
	int error;

	if (statep == NULL)
		return (EINVAL);
	sco = statep;
	error = pthread_rwlock_wrlock(&sco->lock);
	if (error != 0)
		return (error);
	currently_sealed =
	    (sco->sb.readonly_compatible_features & SCO_RO_COMPAT_SEALED) != 0;
	if ((sco->sb.readonly_compatible_features & ~SCO_RO_COMPAT_SUPPORTED) !=
	    0) {
		error = ENOTSUP;
		goto out;
	}
	if (currently_sealed == sealed) {
		error = 0;
		goto out;
	}
	if (sealed && sco->readonly) {
		error = EROFS;
		goto out;
	}

	next_sb = sco->sb;
	next_sb.generation++;
	if (sealed)
		next_sb.readonly_compatible_features |= SCO_RO_COMPAT_SEALED;
	else
		next_sb.readonly_compatible_features &= ~SCO_RO_COMPAT_SEALED;
	error = sco_commit_superblock(sco, &next_sb);
out:
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
	sco_trace_report(sco, "close");
	if (sco->fd >= 0 && close(sco->fd) != 0)
		error = errno;
	free(sco->base_uri);
	free(sco->data_cluster_used);
	free(sco->table);
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
