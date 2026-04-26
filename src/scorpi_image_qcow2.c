/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/errno.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <support/endian.h>
#include <zlib.h>

#include "scorpi_image_qcow2.h"

#define	QCOW2_MAGIC			0x514649fbU
#define	QCOW2_VERSION_2			2U
#define	QCOW2_VERSION_3			3U
#define	QCOW2_HEADER_V2_SIZE		72U
#define	QCOW2_HEADER_V3_SIZE		104U
#define	QCOW2_MAX_BACKING_FILE_SIZE	1023U
#define	QCOW2_MAX_L1_SIZE_BYTES		(32U * 1024U * 1024U)
#define	QCOW2_MIN_CLUSTER_BITS		9U
#define	QCOW2_MAX_CLUSTER_BITS		21U

#define	QCOW2_INCOMPAT_SUPPORTED	0ULL

#define	QCOW2_ENTRY_ZERO		(1ULL << 0)
#define	QCOW2_ENTRY_COMPRESSED		(1ULL << 62)
#define	QCOW2_ENTRY_COPIED		(1ULL << 63)
#define	QCOW2_ENTRY_OFFSET_MASK		0x00ffffffffffffe00ULL
#define	QCOW2_COMPRESSION_TYPE_ZLIB	0U
#define	QCOW2_COMPRESSED_SECTOR_SIZE	512U
#define	QCOW2_L1_RESERVED_MASK \
	(~(QCOW2_ENTRY_OFFSET_MASK | QCOW2_ENTRY_COPIED))
#define	QCOW2_L2_RESERVED_MASK \
	(~(QCOW2_ENTRY_OFFSET_MASK | QCOW2_ENTRY_ZERO | \
	QCOW2_ENTRY_COMPRESSED | QCOW2_ENTRY_COPIED))

struct qcow2_header {
	uint32_t version;
	uint64_t backing_file_offset;
	uint32_t backing_file_size;
	uint32_t cluster_bits;
	uint64_t size;
	uint32_t crypt_method;
	uint32_t l1_size;
	uint64_t l1_table_offset;
	uint32_t nb_snapshots;
	uint64_t incompatible_features;
	uint32_t header_length;
	uint8_t compression_type;
};

struct qcow2_resolved_cluster {
	enum scorpi_image_extent_state state;
	bool compressed;
	uint64_t physical_offset;
	uint64_t compressed_length;
	uint64_t guest_cluster;
};

struct qcow2_image {
	int fd;
	uint64_t file_size;
	uint32_t version;
	uint32_t cluster_bits;
	uint32_t cluster_size;
	uint8_t compression_type;
	uint64_t virtual_size;
	uint32_t l1_size;
	uint64_t *l1_table;
	char *base_uri;

	pthread_mutex_t l2_cache_mtx;
	bool l2_cache_valid;
	uint64_t l2_cache_offset;
	uint64_t *l2_cache;

	pthread_mutex_t compressed_cache_mtx;
	bool compressed_cache_valid;
	uint64_t compressed_cache_guest_cluster;
	uint8_t *compressed_cache;
};

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

static bool
is_cluster_aligned(const struct qcow2_image *qcow2, uint64_t offset)
{
	return ((offset & ((uint64_t)qcow2->cluster_size - 1)) == 0);
}

static int
qcow2_decode_header(int fd, struct qcow2_header *header)
{
	uint8_t buf[QCOW2_HEADER_V3_SIZE];
	uint32_t magic, header_length;
	int error;

	memset(header, 0, sizeof(*header));
	error = read_exact(fd, 0, buf, QCOW2_HEADER_V2_SIZE);
	if (error != 0)
		return (error);
	magic = be32dec(buf + 0);
	if (magic != QCOW2_MAGIC)
		return (EINVAL);

	header->version = be32dec(buf + 4);
	if (header->version != QCOW2_VERSION_2 &&
	    header->version != QCOW2_VERSION_3)
		return (ENOTSUP);

	header->backing_file_offset = be64dec(buf + 8);
	header->backing_file_size = be32dec(buf + 16);
	header->cluster_bits = be32dec(buf + 20);
	header->size = be64dec(buf + 24);
	header->crypt_method = be32dec(buf + 32);
	header->l1_size = be32dec(buf + 36);
	header->l1_table_offset = be64dec(buf + 40);
	header->nb_snapshots = be32dec(buf + 60);

	if (header->version == QCOW2_VERSION_2) {
		header->header_length = QCOW2_HEADER_V2_SIZE;
		return (0);
	}

	error = read_exact(fd, QCOW2_HEADER_V2_SIZE,
	    buf + QCOW2_HEADER_V2_SIZE,
	    QCOW2_HEADER_V3_SIZE - QCOW2_HEADER_V2_SIZE);
	if (error != 0)
		return (error);
	header->incompatible_features = be64dec(buf + 72);
	header_length = be32dec(buf + 100);
	if (header_length < QCOW2_HEADER_V3_SIZE ||
	    (header_length % 8) != 0)
		return (EINVAL);
	header->header_length = header_length;
	if (header_length > 104) {
		error = read_exact(fd, 104, &header->compression_type, 1);
		if (error != 0)
			return (error);
	}
	return (0);
}

static int
qcow2_probe(int fd, uint32_t *score)
{
	uint8_t buf[4];

	if (score == NULL)
		return (EINVAL);
	*score = 0;
	if (fd < 0)
		return (0);
	if (pread(fd, buf, sizeof(buf), 0) != (ssize_t)sizeof(buf))
		return (0);
	if (be32dec(buf) == QCOW2_MAGIC)
		*score = 90;
	return (0);
}

static int
qcow2_make_base_uri(const char *backing, char **base_urip)
{
	const char *prefix;
	char *uri;
	size_t len;

	if (backing == NULL || backing[0] == '\0' || base_urip == NULL)
		return (EINVAL);
	if (strncmp(backing, "file:", 5) == 0) {
		uri = strdup(backing);
		if (uri == NULL)
			return (ENOMEM);
		*base_urip = uri;
		return (0);
	}
	if (strchr(backing, ':') != NULL)
		return (ENOTSUP);

	prefix = backing[0] == '/' ? "file://" : "file:";
	len = strlen(prefix) + strlen(backing) + 1;
	uri = malloc(len);
	if (uri == NULL)
		return (ENOMEM);
	snprintf(uri, len, "%s%s", prefix, backing);
	*base_urip = uri;
	return (0);
}

static int
qcow2_read_base_uri(int fd, const struct qcow2_header *header,
    char **base_urip)
{
	char backing[QCOW2_MAX_BACKING_FILE_SIZE + 1];
	int error;

	if (header->backing_file_offset == 0) {
		if (header->backing_file_size != 0)
			return (EINVAL);
		*base_urip = NULL;
		return (0);
	}
	if (header->backing_file_size == 0 ||
	    header->backing_file_size > QCOW2_MAX_BACKING_FILE_SIZE)
		return (EINVAL);
	if (header->backing_file_offset >
	    UINT64_MAX - header->backing_file_size)
		return (EFBIG);

	error = read_exact(fd, header->backing_file_offset, backing,
	    header->backing_file_size);
	if (error != 0)
		return (error);
	backing[header->backing_file_size] = '\0';
	return (qcow2_make_base_uri(backing, base_urip));
}

static int
qcow2_validate_header(const struct qcow2_header *header)
{
	uint64_t cluster_size, l1_bytes, l2_entries, covered_clusters;

	if (header->cluster_bits < QCOW2_MIN_CLUSTER_BITS ||
	    header->cluster_bits > QCOW2_MAX_CLUSTER_BITS)
		return (ENOTSUP);
	cluster_size = 1ULL << header->cluster_bits;
	if (header->header_length > cluster_size)
		return (EINVAL);
	if (header->crypt_method != 0)
		return (ENOTSUP);
	if ((header->incompatible_features & ~QCOW2_INCOMPAT_SUPPORTED) != 0)
		return (ENOTSUP);
	if (header->compression_type != QCOW2_COMPRESSION_TYPE_ZLIB)
		return (ENOTSUP);
	if (header->l1_size == 0 || header->l1_table_offset == 0)
		return (EINVAL);
	if (header->l1_size > QCOW2_MAX_L1_SIZE_BYTES / sizeof(uint64_t))
		return (EFBIG);
	l1_bytes = (uint64_t)header->l1_size * sizeof(uint64_t);
	if ((header->l1_table_offset % cluster_size) != 0)
		return (EINVAL);
	if (header->l1_table_offset > UINT64_MAX - l1_bytes)
		return (EFBIG);

	l2_entries = cluster_size / sizeof(uint64_t);
	if (header->size > 0) {
		covered_clusters = (uint64_t)header->l1_size * l2_entries;
		if ((header->size + cluster_size - 1) / cluster_size >
		    covered_clusters)
			return (EINVAL);
	}
	return (0);
}

static int
qcow2_read_l1_table(struct qcow2_image *qcow2,
    const struct qcow2_header *header)
{
	uint8_t *buf;
	size_t len, i;
	uint64_t entry, offset;
	int error;

	len = (size_t)header->l1_size * sizeof(uint64_t);
	buf = malloc(len);
	if (buf == NULL)
		return (ENOMEM);
	error = read_exact(qcow2->fd, header->l1_table_offset, buf, len);
	if (error != 0) {
		free(buf);
		return (error);
	}
	qcow2->l1_table = calloc(header->l1_size, sizeof(qcow2->l1_table[0]));
	if (qcow2->l1_table == NULL) {
		free(buf);
		return (ENOMEM);
	}
	for (i = 0; i < header->l1_size; i++) {
		entry = be64dec(buf + i * sizeof(uint64_t));
		if (entry == 0) {
			qcow2->l1_table[i] = 0;
			continue;
		}
		if ((entry & QCOW2_L1_RESERVED_MASK) != 0) {
			free(buf);
			return (ENOTSUP);
		}
		offset = entry & QCOW2_ENTRY_OFFSET_MASK;
		if (offset == 0 || !is_cluster_aligned(qcow2, offset) ||
		    offset > qcow2->file_size ||
		    (uint64_t)qcow2->cluster_size > qcow2->file_size - offset) {
			free(buf);
			return (EINVAL);
		}
		qcow2->l1_table[i] = offset;
	}
	free(buf);
	return (0);
}

static int
qcow2_open(const char *path __attribute__((unused)), int fd, bool readonly,
    void **statep)
{
	struct qcow2_header header;
	struct qcow2_image *qcow2;
	struct stat sb;
	int error;

	if (statep == NULL || fd < 0)
		return (EINVAL);
	*statep = NULL;
	if (!readonly)
		return (EROFS);
	if (fstat(fd, &sb) != 0)
		return (errno);
	error = qcow2_decode_header(fd, &header);
	if (error != 0)
		return (error);
	error = qcow2_validate_header(&header);
	if (error != 0)
		return (error);

	qcow2 = calloc(1, sizeof(*qcow2));
	if (qcow2 == NULL)
		return (ENOMEM);
	qcow2->fd = fd;
	qcow2->file_size = (uint64_t)sb.st_size;
	qcow2->version = header.version;
	qcow2->cluster_bits = header.cluster_bits;
	qcow2->cluster_size = 1U << header.cluster_bits;
	qcow2->compression_type = header.compression_type;
	qcow2->virtual_size = header.size;
	qcow2->l1_size = header.l1_size;

	error = pthread_mutex_init(&qcow2->l2_cache_mtx, NULL);
	if (error != 0)
		goto err;
	error = pthread_mutex_init(&qcow2->compressed_cache_mtx, NULL);
	if (error != 0)
		goto err_destroy_l2_mtx;
	qcow2->l2_cache = calloc(qcow2->cluster_size, 1);
	if (qcow2->l2_cache == NULL) {
		error = ENOMEM;
		goto err_destroy_compressed_mtx;
	}
	qcow2->compressed_cache = malloc(qcow2->cluster_size);
	if (qcow2->compressed_cache == NULL) {
		error = ENOMEM;
		goto err_destroy_compressed_mtx;
	}
	error = qcow2_read_l1_table(qcow2, &header);
	if (error != 0)
		goto err_destroy_compressed_mtx;
	error = qcow2_read_base_uri(fd, &header, &qcow2->base_uri);
	if (error != 0)
		goto err_destroy_compressed_mtx;

	*statep = qcow2;
	return (0);

err_destroy_compressed_mtx:
	pthread_mutex_destroy(&qcow2->compressed_cache_mtx);
err_destroy_l2_mtx:
	pthread_mutex_destroy(&qcow2->l2_cache_mtx);
err:
	free(qcow2->base_uri);
	free(qcow2->l1_table);
	free(qcow2->l2_cache);
	free(qcow2->compressed_cache);
	free(qcow2);
	return (error);
}

static int
qcow2_get_info(void *statep, struct scorpi_image_info *info)
{
	struct qcow2_image *qcow2;

	if (statep == NULL || info == NULL)
		return (EINVAL);

	qcow2 = statep;
	*info = (struct scorpi_image_info){
		.format = SCORPI_IMAGE_FORMAT_QCOW2,
		.virtual_size = qcow2->virtual_size,
		.logical_sector_size = 512,
		.physical_sector_size = 0,
		.cluster_size = qcow2->cluster_size,
		.readonly = true,
		.sealed = true,
		.has_base = qcow2->base_uri != NULL,
	};
	if (qcow2->base_uri != NULL) {
		info->base_uri = strdup(qcow2->base_uri);
		if (info->base_uri == NULL)
			return (ENOMEM);
	}
	return (0);
}

static int
qcow2_l2_entry(struct qcow2_image *qcow2, uint64_t l2_offset,
    uint64_t l2_index, uint64_t *entryp)
{
	uint8_t *buf;
	uint64_t l2_entries;
	size_t i;
	int error;

	if (l2_offset == 0) {
		*entryp = 0;
		return (0);
	}
	if (!is_cluster_aligned(qcow2, l2_offset) ||
	    l2_offset > qcow2->file_size ||
	    (uint64_t)qcow2->cluster_size > qcow2->file_size - l2_offset)
		return (EINVAL);

	l2_entries = qcow2->cluster_size / sizeof(uint64_t);
	if (l2_index >= l2_entries)
		return (EINVAL);

	error = pthread_mutex_lock(&qcow2->l2_cache_mtx);
	if (error != 0)
		return (error);
	if (qcow2->l2_cache_valid && qcow2->l2_cache_offset == l2_offset) {
		*entryp = qcow2->l2_cache[l2_index];
		pthread_mutex_unlock(&qcow2->l2_cache_mtx);
		return (0);
	}

	buf = malloc(qcow2->cluster_size);
	if (buf == NULL) {
		pthread_mutex_unlock(&qcow2->l2_cache_mtx);
		return (ENOMEM);
	}
	error = read_exact(qcow2->fd, l2_offset, buf, qcow2->cluster_size);
	if (error == 0) {
		for (i = 0; i < l2_entries; i++)
			qcow2->l2_cache[i] = be64dec(buf +
			    i * sizeof(uint64_t));
		qcow2->l2_cache_offset = l2_offset;
		qcow2->l2_cache_valid = true;
		*entryp = qcow2->l2_cache[l2_index];
	}
	free(buf);
	pthread_mutex_unlock(&qcow2->l2_cache_mtx);
	return (error);
}

static int
qcow2_resolve_cluster(struct qcow2_image *qcow2, uint64_t offset,
    struct qcow2_resolved_cluster *resolved)
{
	uint64_t guest_cluster, l2_entries, l1_index, l2_index;
	uint64_t l2_offset, entry, physical_offset, compressed_sectors;
	unsigned int compressed_offset_bits;
	int error;

	if (resolved == NULL || offset >= qcow2->virtual_size)
		return (EINVAL);
	memset(resolved, 0, sizeof(*resolved));
	guest_cluster = offset >> qcow2->cluster_bits;
	resolved->guest_cluster = guest_cluster;
	l2_entries = qcow2->cluster_size / sizeof(uint64_t);
	l1_index = guest_cluster / l2_entries;
	l2_index = guest_cluster % l2_entries;
	if (l1_index >= qcow2->l1_size)
		return (EINVAL);

	l2_offset = qcow2->l1_table[l1_index];
	if (l2_offset == 0) {
		resolved->state = SCORPI_IMAGE_EXTENT_ABSENT;
		return (0);
	}

	error = qcow2_l2_entry(qcow2, l2_offset, l2_index, &entry);
	if (error != 0)
		return (error);
	if ((entry & QCOW2_ENTRY_COMPRESSED) != 0) {
		if ((entry & QCOW2_ENTRY_COPIED) != 0)
			return (EINVAL);
		compressed_offset_bits = 62U - (qcow2->cluster_bits - 8U);
		physical_offset = entry & ((1ULL << compressed_offset_bits) - 1);
		compressed_sectors = (entry >> compressed_offset_bits) &
		    ((1ULL << (62U - compressed_offset_bits)) - 1);
		if (compressed_sectors == UINT64_MAX ||
		    compressed_sectors + 1 >
		    UINT64_MAX / QCOW2_COMPRESSED_SECTOR_SIZE)
			return (EINVAL);
		resolved->compressed_length = (compressed_sectors + 1) *
		    QCOW2_COMPRESSED_SECTOR_SIZE -
		    (physical_offset & (QCOW2_COMPRESSED_SECTOR_SIZE - 1));
		if (resolved->compressed_length == 0)
			return (EINVAL);
		if (physical_offset > qcow2->file_size ||
		    resolved->compressed_length >
		    qcow2->file_size - physical_offset)
			return (EINVAL);
		resolved->state = SCORPI_IMAGE_EXTENT_PRESENT;
		resolved->compressed = true;
		resolved->physical_offset = physical_offset;
		return (0);
	}
	if ((entry & QCOW2_L2_RESERVED_MASK) != 0)
		return (ENOTSUP);
	if (qcow2->version == QCOW2_VERSION_2 &&
	    (entry & QCOW2_ENTRY_ZERO) != 0)
		return (ENOTSUP);
	if ((entry & QCOW2_ENTRY_ZERO) != 0) {
		resolved->state = SCORPI_IMAGE_EXTENT_ZERO;
		return (0);
	}

	physical_offset = entry & QCOW2_ENTRY_OFFSET_MASK;
	if (physical_offset == 0) {
		if ((entry & QCOW2_ENTRY_COPIED) != 0)
			return (EINVAL);
		resolved->state = SCORPI_IMAGE_EXTENT_ABSENT;
		return (0);
	}
	if (!is_cluster_aligned(qcow2, physical_offset) ||
	    physical_offset > qcow2->file_size)
		return (EINVAL);
	resolved->state = SCORPI_IMAGE_EXTENT_PRESENT;
	resolved->physical_offset = physical_offset;
	return (0);
}

static int
qcow2_map(void *statep, uint64_t offset, uint64_t length,
    struct scorpi_image_extent *extent)
{
	struct qcow2_image *qcow2;
	struct qcow2_resolved_cluster resolved;
	uint64_t cluster_start, cluster_end;
	int error;

	if (statep == NULL || extent == NULL || length == 0)
		return (EINVAL);
	qcow2 = statep;
	if (offset >= qcow2->virtual_size)
		return (EINVAL);
	if (length > qcow2->virtual_size - offset)
		length = qcow2->virtual_size - offset;

	error = qcow2_resolve_cluster(qcow2, offset, &resolved);
	if (error != 0)
		return (error);
	cluster_start = offset & ~((uint64_t)qcow2->cluster_size - 1);
	cluster_end = cluster_start + qcow2->cluster_size;
	if (cluster_end > qcow2->virtual_size)
		cluster_end = qcow2->virtual_size;
	if (length > cluster_end - offset)
		length = cluster_end - offset;

	*extent = (struct scorpi_image_extent){
		.offset = offset,
		.length = length,
		.state = resolved.state,
	};
	return (0);
}

static int
qcow2_read_compressed_cluster_locked(struct qcow2_image *qcow2,
    const struct qcow2_resolved_cluster *resolved)
{
	uint8_t *compressed;
	z_stream zs;
	int error, zret;

	if (qcow2->compressed_cache_valid &&
	    qcow2->compressed_cache_guest_cluster == resolved->guest_cluster)
		return (0);
	if (qcow2->compression_type != QCOW2_COMPRESSION_TYPE_ZLIB)
		return (ENOTSUP);
	if (resolved->compressed_length > UINT_MAX)
		return (EFBIG);

	compressed = malloc((size_t)resolved->compressed_length);
	if (compressed == NULL)
		return (ENOMEM);
	error = read_exact(qcow2->fd, resolved->physical_offset, compressed,
	    (size_t)resolved->compressed_length);
	if (error != 0) {
		free(compressed);
		return (error);
	}

	memset(&zs, 0, sizeof(zs));
	zret = inflateInit2(&zs, -MAX_WBITS);
	if (zret != Z_OK) {
		free(compressed);
		return (EIO);
	}
	zs.next_in = compressed;
	zs.avail_in = (uInt)resolved->compressed_length;
	zs.next_out = qcow2->compressed_cache;
	zs.avail_out = qcow2->cluster_size;
	zret = inflate(&zs, Z_FINISH);
	if (zret != Z_STREAM_END || zs.total_out != qcow2->cluster_size) {
		inflateEnd(&zs);
		free(compressed);
		return (EIO);
	}
	if (inflateEnd(&zs) != Z_OK) {
		free(compressed);
		return (EIO);
	}

	qcow2->compressed_cache_guest_cluster = resolved->guest_cluster;
	qcow2->compressed_cache_valid = true;
	free(compressed);
	return (0);
}

static int
qcow2_read_compressed_cluster(struct qcow2_image *qcow2,
    const struct qcow2_resolved_cluster *resolved, void *buf,
    uint64_t cluster_offset, size_t len)
{
	int error;

	error = pthread_mutex_lock(&qcow2->compressed_cache_mtx);
	if (error != 0)
		return (error);
	error = qcow2_read_compressed_cluster_locked(qcow2, resolved);
	if (error == 0)
		memcpy(buf, qcow2->compressed_cache + cluster_offset, len);
	pthread_mutex_unlock(&qcow2->compressed_cache_mtx);
	return (error);
}

static int
qcow2_read(void *statep, void *buf, uint64_t offset, size_t len)
{
	struct qcow2_image *qcow2;
	struct qcow2_resolved_cluster resolved;
	uint8_t *p;
	uint64_t cluster_offset, chunk;
	int error;

	if (statep == NULL || (buf == NULL && len != 0))
		return (EINVAL);
	if (len == 0)
		return (0);
	qcow2 = statep;
	if (offset > qcow2->virtual_size ||
	    (uint64_t)len > qcow2->virtual_size - offset)
		return (EINVAL);

	p = buf;
	while (len > 0) {
		error = qcow2_resolve_cluster(qcow2, offset, &resolved);
		if (error != 0)
			return (error);
		cluster_offset = offset & ((uint64_t)qcow2->cluster_size - 1);
		chunk = (uint64_t)qcow2->cluster_size - cluster_offset;
		if (chunk > len)
			chunk = len;

		switch (resolved.state) {
		case SCORPI_IMAGE_EXTENT_PRESENT:
			if (resolved.compressed) {
				error = qcow2_read_compressed_cluster(qcow2,
				    &resolved, p, cluster_offset, (size_t)chunk);
				if (error != 0)
					return (error);
				break;
			}
			if (resolved.physical_offset > UINT64_MAX - cluster_offset ||
			    resolved.physical_offset + cluster_offset >
			    qcow2->file_size ||
			    chunk > qcow2->file_size -
			    (resolved.physical_offset + cluster_offset))
				return (EINVAL);
			error = read_exact(qcow2->fd,
			    resolved.physical_offset + cluster_offset, p,
			    (size_t)chunk);
			if (error != 0)
				return (error);
			break;
		case SCORPI_IMAGE_EXTENT_ZERO:
		case SCORPI_IMAGE_EXTENT_ABSENT:
		case SCORPI_IMAGE_EXTENT_DISCARDED:
			memset(p, 0, (size_t)chunk);
			break;
		default:
			return (EINVAL);
		}
		p += chunk;
		offset += chunk;
		len -= (size_t)chunk;
	}
	return (0);
}

static int
qcow2_write(void *statep, const void *buf __attribute__((unused)),
    uint64_t offset __attribute__((unused)), size_t len __attribute__((unused)))
{
	if (statep == NULL)
		return (EINVAL);
	return (EROFS);
}

static int
qcow2_discard(void *statep, uint64_t offset __attribute__((unused)),
    uint64_t length __attribute__((unused)))
{
	if (statep == NULL)
		return (EINVAL);
	return (EROFS);
}

static int
qcow2_flush(void *statep)
{
	if (statep == NULL)
		return (EINVAL);
	return (0);
}

static int
qcow2_close(void *statep)
{
	struct qcow2_image *qcow2;
	int error;

	if (statep == NULL)
		return (0);
	qcow2 = statep;
	error = 0;
	if (qcow2->fd >= 0 && close(qcow2->fd) != 0)
		error = errno;
	pthread_mutex_destroy(&qcow2->compressed_cache_mtx);
	pthread_mutex_destroy(&qcow2->l2_cache_mtx);
	free(qcow2->base_uri);
	free(qcow2->l1_table);
	free(qcow2->l2_cache);
	free(qcow2->compressed_cache);
	free(qcow2);
	return (error);
}

static const struct scorpi_image_ops scorpi_qcow2_ops = {
	.name = "qcow2",
	.probe = qcow2_probe,
	.open = qcow2_open,
	.get_info = qcow2_get_info,
	.map = qcow2_map,
	.read = qcow2_read,
	.write = qcow2_write,
	.discard = qcow2_discard,
	.flush = qcow2_flush,
	.close = qcow2_close,
};
SCORPI_IMAGE_BACKEND_SET(scorpi_qcow2_ops);

const struct scorpi_image_ops *
scorpi_image_qcow2_backend(void)
{
	return (&scorpi_qcow2_ops);
}
