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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libutil.h>
#include <support/endian.h>

#include "scorpi_image.h"
#include "scorpi_image_sco.h"

#define	SCO_MAGIC			"SCOIMG\0\0"
#define	SCO_FILE_ID_SIZE		0x1000U
#define	SCO_SUPERBLOCK_SIZE		0x1000U
#define	SCO_SUPERBLOCK_A_OFFSET		0x1000ULL
#define	SCO_SUPERBLOCK_B_OFFSET		0x2000ULL
#define	SCO_METADATA_AREA_OFFSET		0x10000ULL
#define	SCO_METADATA_PAGE_SIZE		0x1000U
#define	SCO_MAP_PAGE_HEADER_SIZE		0x18U
#define	SCO_MAP_ENTRY_SIZE		16U
#define	SCO_MAP_ENTRIES_PER_PAGE \
	((SCO_METADATA_PAGE_SIZE - SCO_MAP_PAGE_HEADER_SIZE) / \
	SCO_MAP_ENTRY_SIZE)
#define	SCO_CHECKSUM_CRC32C		1U
#define	SCO_DEFAULT_CLUSTER_SIZE		0x40000U
#define	SCO_MIN_CLUSTER_SIZE		0x10000U
#define	SCO_MAX_CLUSTER_SIZE		0x400000U
#define	SCO_INCOMPAT_ALLOC_MAP_V1	(1U << 0)

struct sco_create_options {
	const char *path;
	const char *base_uri;
	uint64_t virtual_size;
	uint32_t cluster_size;
	uint32_t logical_sector_size;
	uint32_t physical_sector_size;
};

static uint32_t
crc32c(const void *buf, size_t len)
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

static uint64_t
align_up_u64(uint64_t value, uint64_t alignment)
{
	return ((value + alignment - 1) & ~(alignment - 1));
}

static bool
is_power_of_two_u64(uint64_t value)
{
	return (value != 0 && (value & (value - 1)) == 0);
}

static int
write_exact(int fd, uint64_t offset, const void *buf, size_t len)
{
	ssize_t n;

	n = pwrite(fd, buf, len, (off_t)offset);
	if (n < 0)
		return (errno);
	if ((size_t)n != len)
		return (EIO);
	return (0);
}

static int
fill_uuid(uint8_t uuid[16])
{
	struct timespec ts;
	int fd;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) {
		if (read(fd, uuid, 16) == 16) {
			close(fd);
			uuid[6] = (uuid[6] & 0x0f) | 0x40;
			uuid[8] = (uuid[8] & 0x3f) | 0x80;
			return (0);
		}
		close(fd);
	}
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
		return (errno);
	memset(uuid, 0, 16);
	le64enc(uuid, (uint64_t)ts.tv_sec);
	le64enc(uuid + 8, (uint64_t)ts.tv_nsec);
	uuid[6] = (uuid[6] & 0x0f) | 0x40;
	uuid[8] = (uuid[8] & 0x3f) | 0x80;
	return (0);
}

static void
build_file_id(uint8_t buf[SCO_FILE_ID_SIZE], const uint8_t uuid[16])
{
	uint32_t crc;

	memset(buf, 0, SCO_FILE_ID_SIZE);
	memcpy(buf, SCO_MAGIC, 8);
	le16enc(buf + 0x0008, 1);
	le32enc(buf + 0x000c, SCO_FILE_ID_SIZE);
	memcpy(buf + 0x0010, uuid, 16);
	le32enc(buf + 0x0020, SCO_CHECKSUM_CRC32C);
	crc = crc32c(buf, SCO_FILE_ID_SIZE);
	le32enc(buf + 0x0024, crc);
}

static void
build_root_page(uint8_t buf[SCO_METADATA_PAGE_SIZE], uint32_t entry_count,
    uint64_t first_root_index)
{
	uint32_t crc;

	memset(buf, 0, SCO_METADATA_PAGE_SIZE);
	le32enc(buf + 0x0000, SCO_METADATA_PAGE_SIZE);
	le32enc(buf + 0x0004, SCO_CHECKSUM_CRC32C);
	le32enc(buf + 0x000c, entry_count);
	le64enc(buf + 0x0010, first_root_index);
	crc = crc32c(buf, SCO_METADATA_PAGE_SIZE);
	le32enc(buf + 0x0008, crc);
}

static void
build_base_descriptor(uint8_t *buf, uint32_t size, const char *base_uri)
{
	uint32_t crc;
	size_t uri_len;

	memset(buf, 0, size);
	uri_len = strlen(base_uri);
	le32enc(buf + 0x0000, size);
	le32enc(buf + 0x0004, SCO_CHECKSUM_CRC32C);
	le32enc(buf + 0x0010, (uint32_t)uri_len);
	memcpy(buf + 0x0050, base_uri, uri_len);
	crc = crc32c(buf, size);
	le32enc(buf + 0x0008, crc);
}

static void
build_superblock(uint8_t buf[SCO_SUPERBLOCK_SIZE],
    const struct sco_create_options *opts, const uint8_t uuid[16],
    uint64_t cluster_count, uint64_t data_area_offset,
    uint64_t base_descriptor_offset, uint32_t base_descriptor_length,
    uint64_t map_root_offset, uint32_t map_root_length)
{
	uint32_t crc;

	memset(buf, 0, SCO_SUPERBLOCK_SIZE);
	memcpy(buf, SCO_MAGIC, 8);
	le16enc(buf + 0x0008, 1);
	le32enc(buf + 0x000c, SCO_SUPERBLOCK_SIZE);
	le32enc(buf + 0x0010, SCO_CHECKSUM_CRC32C);
	le64enc(buf + 0x0018, 1);
	le64enc(buf + 0x0020, opts->virtual_size);
	le32enc(buf + 0x0028, opts->logical_sector_size);
	le32enc(buf + 0x002c, opts->physical_sector_size);
	le32enc(buf + 0x0030, opts->cluster_size);
	le64enc(buf + 0x0038, cluster_count);
	le64enc(buf + 0x0040, SCO_METADATA_AREA_OFFSET);
	le64enc(buf + 0x0048, data_area_offset);
	le64enc(buf + 0x0050, base_descriptor_offset);
	le32enc(buf + 0x0058, base_descriptor_length);
	le64enc(buf + 0x0060, map_root_offset);
	le32enc(buf + 0x0068, map_root_length);
	le32enc(buf + 0x006c, SCO_MAP_ENTRY_SIZE);
	le32enc(buf + 0x0070, SCO_INCOMPAT_ALLOC_MAP_V1);
	memcpy(buf + 0x0080, uuid, 16);
	crc = crc32c(buf, SCO_SUPERBLOCK_SIZE);
	le32enc(buf + 0x0014, crc);
}

static int
create_sco(const struct sco_create_options *opts)
{
	uint8_t page[SCO_METADATA_PAGE_SIZE], uuid[16];
	uint8_t *base_descriptor;
	uint64_t cluster_count, map_page_count, root_page_count;
	uint64_t base_descriptor_offset, data_area_offset, metadata_end;
	uint64_t metadata_dynamic_pages;
	uint32_t base_descriptor_length, root_entries, map_root_length;
	uint64_t i, remaining_root_entries;
	int fd, error;

	if (opts->path == NULL || opts->virtual_size == 0 ||
	    opts->logical_sector_size == 0 || opts->cluster_size == 0)
		return (EINVAL);
	if (!is_power_of_two_u64(opts->logical_sector_size) ||
	    !is_power_of_two_u64(opts->cluster_size) ||
	    opts->cluster_size < SCO_MIN_CLUSTER_SIZE ||
	    opts->cluster_size > SCO_MAX_CLUSTER_SIZE ||
	    (opts->cluster_size % opts->logical_sector_size) != 0)
		return (EINVAL);
	if (opts->physical_sector_size != 0 &&
	    (!is_power_of_two_u64(opts->physical_sector_size) ||
	    opts->physical_sector_size < opts->logical_sector_size))
		return (EINVAL);

	cluster_count = (opts->virtual_size + opts->cluster_size - 1) /
	    opts->cluster_size;
	map_page_count = (cluster_count + SCO_MAP_ENTRIES_PER_PAGE - 1) /
	    SCO_MAP_ENTRIES_PER_PAGE;
	root_page_count = (map_page_count + SCO_MAP_ENTRIES_PER_PAGE - 1) /
	    SCO_MAP_ENTRIES_PER_PAGE;
	if (root_page_count > UINT32_MAX / SCO_METADATA_PAGE_SIZE)
		return (EFBIG);
	map_root_length = (uint32_t)(root_page_count * SCO_METADATA_PAGE_SIZE);
	metadata_end = SCO_METADATA_AREA_OFFSET + map_root_length;
	base_descriptor_offset = 0;
	base_descriptor_length = 0;
	if (opts->base_uri != NULL) {
		base_descriptor_offset = metadata_end;
		base_descriptor_length = (uint32_t)align_up_u64(0x50 +
		    strlen(opts->base_uri), SCO_METADATA_PAGE_SIZE);
		if (base_descriptor_length > UINT64_MAX - metadata_end)
			return (EFBIG);
		metadata_end += base_descriptor_length;
	}
	if (map_page_count > UINT64_MAX - root_page_count - 1)
		return (EFBIG);
	metadata_dynamic_pages = map_page_count + root_page_count + 1;
	if (metadata_dynamic_pages >
	    (UINT64_MAX - metadata_end) / SCO_METADATA_PAGE_SIZE)
		return (EFBIG);
	metadata_end += metadata_dynamic_pages * SCO_METADATA_PAGE_SIZE;
	if (metadata_end > UINT64_MAX - opts->cluster_size + 1)
		return (EFBIG);
	data_area_offset = align_up_u64(metadata_end, opts->cluster_size);

	error = fill_uuid(uuid);
	if (error != 0)
		return (error);
	fd = open(opts->path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	if (fd < 0)
		return (errno);

	build_file_id(page, uuid);
	error = write_exact(fd, 0, page, sizeof(page));
	if (error != 0)
		goto out;
	build_superblock(page, opts, uuid, cluster_count, data_area_offset,
	    base_descriptor_offset, base_descriptor_length,
	    SCO_METADATA_AREA_OFFSET, map_root_length);
	error = write_exact(fd, SCO_SUPERBLOCK_A_OFFSET, page, sizeof(page));
	if (error != 0)
		goto out;
	memset(page, 0, sizeof(page));
	error = write_exact(fd, SCO_SUPERBLOCK_B_OFFSET, page, sizeof(page));
	if (error != 0)
		goto out;

	remaining_root_entries = map_page_count;
	for (i = 0; i < root_page_count; i++) {
		root_entries = remaining_root_entries > SCO_MAP_ENTRIES_PER_PAGE ?
		    SCO_MAP_ENTRIES_PER_PAGE : (uint32_t)remaining_root_entries;
		build_root_page(page, root_entries,
		    i * SCO_MAP_ENTRIES_PER_PAGE);
		error = write_exact(fd, SCO_METADATA_AREA_OFFSET +
		    i * SCO_METADATA_PAGE_SIZE, page, sizeof(page));
		if (error != 0)
			goto out;
		remaining_root_entries -= root_entries;
	}
	if (opts->base_uri != NULL) {
		base_descriptor = calloc(1, base_descriptor_length);
		if (base_descriptor == NULL) {
			error = ENOMEM;
			goto out;
		}
		build_base_descriptor(base_descriptor, base_descriptor_length,
		    opts->base_uri);
		error = write_exact(fd, base_descriptor_offset,
		    base_descriptor, base_descriptor_length);
		free(base_descriptor);
		if (error != 0)
			goto out;
	}
	if (ftruncate(fd, (off_t)data_area_offset) != 0) {
		error = errno;
		goto out;
	}
	error = 0;
out:
	if (close(fd) != 0 && error == 0)
		error = errno;
	return (error);
}

static int
open_image_top(const char *path, const struct scorpi_image_ops **opsp,
    void **statep)
{
	const struct scorpi_image_ops *ops;
	uint32_t score;
	int fd, error;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return (errno);
	ops = NULL;
	error = scorpi_image_backend_probe(fd, &ops, &score);
	if (error != 0) {
		close(fd);
		return (error);
	}
	if (ops == NULL || score == 0) {
		close(fd);
		return (ENOENT);
	}
	error = ops->open(path, fd, true, statep);
	if (error != 0) {
		close(fd);
		return (error);
	}
	*opsp = ops;
	return (0);
}

static int
open_sco_for_seal_state(const char *path, bool readonly_open,
    const struct scorpi_image_ops **opsp, void **statep)
{
	const struct scorpi_image_ops *ops;
	uint32_t score;
	int fd, error;

	fd = open(path, O_RDWR);
	if (fd < 0)
		return (errno);
	ops = NULL;
	error = scorpi_image_backend_probe(fd, &ops, &score);
	if (error != 0) {
		close(fd);
		return (error);
	}
	if (ops == NULL || score == 0 || ops->name == NULL ||
	    strcmp(ops->name, "sco") != 0) {
		close(fd);
		return (ENOTSUP);
	}
	error = ops->open(path, fd, readonly_open, statep);
	if (error != 0) {
		close(fd);
		return (error);
	}
	*opsp = ops;
	return (0);
}

static const char *
format_name(enum scorpi_image_format format)
{
	switch (format) {
	case SCORPI_IMAGE_FORMAT_RAW:
		return ("raw");
	case SCORPI_IMAGE_FORMAT_QCOW2:
		return ("qcow2");
	case SCORPI_IMAGE_FORMAT_SCO:
		return ("sco");
	default:
		return ("unknown");
	}
}

static int
cmd_info(const char *path)
{
	const struct scorpi_image_ops *ops;
	struct scorpi_image_info info;
	void *state;
	int error;

	memset(&info, 0, sizeof(info));
	state = NULL;
	error = open_image_top(path, &ops, &state);
	if (error != 0)
		return (error);
	error = ops->get_info(state, &info);
	if (error == 0) {
		printf("format=%s\n", format_name(info.format));
		printf("virtual_size=%llu\n",
		    (unsigned long long)info.virtual_size);
		printf("logical_sector_size=%u\n", info.logical_sector_size);
		printf("physical_sector_size=%u\n", info.physical_sector_size);
		printf("cluster_size=%u\n", info.cluster_size);
		printf("readonly=%s\n", info.readonly ? "true" : "false");
		printf("sealed=%s\n", info.sealed ? "true" : "false");
		if (info.has_base)
			printf("base_uri=%s\n", info.base_uri);
	}
	free(info.base_uri);
	ops->close(state);
	return (error);
}

static int
cmd_check(const char *path)
{
	const struct scorpi_image_ops *ops;
	void *state;
	int error;

	state = NULL;
	error = open_image_top(path, &ops, &state);
	if (error != 0)
		return (error);
	error = ops->close(state);
	if (error == 0)
		printf("ok\n");
	return (error);
}

static int
cmd_set_sealed(const char *path, bool sealed)
{
	const struct scorpi_image_ops *ops;
	void *state;
	int error, close_error;

	state = NULL;
	error = open_sco_for_seal_state(path, !sealed, &ops, &state);
	if (error == EROFS && sealed)
		error = open_sco_for_seal_state(path, true, &ops, &state);
	if (error != 0)
		return (error);
	error = scorpi_image_sco_set_sealed(state, sealed);
	close_error = ops->close(state);
	if (error == 0)
		error = close_error;
	return (error);
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage:\n"
	    "  scorpi-image create --format sco --size bytes|Nmb|Ngb path [base]\n"
	    "  scorpi-image seal path.sco\n"
	    "  scorpi-image unseal path.sco\n"
	    "  scorpi-image info path\n"
	    "  scorpi-image check path\n");
}

static int
make_file_uri(const char *base, char **urip)
{
	const char *prefix;
	char *uri;
	size_t len;

	if (base == NULL || base[0] == '\0' || urip == NULL)
		return (EINVAL);
	if (strncmp(base, "file:", 5) == 0) {
		uri = strdup(base);
		if (uri == NULL)
			return (ENOMEM);
		*urip = uri;
		return (0);
	}

	prefix = base[0] == '/' ? "file://" : "file:";
	len = strlen(prefix) + strlen(base) + 1;
	uri = malloc(len);
	if (uri == NULL)
		return (ENOMEM);
	snprintf(uri, len, "%s%s", prefix, base);
	*urip = uri;
	return (0);
}

static int
cmd_create(int argc, char **argv)
{
	struct sco_create_options opts;
	char *base_uri;
	const char *format;
	int i, error;

	opts = (struct sco_create_options){
		.cluster_size = SCO_DEFAULT_CLUSTER_SIZE,
		.logical_sector_size = 512,
		.physical_sector_size = 4096,
	};
	base_uri = NULL;
	format = NULL;
	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
			format = argv[++i];
		} else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
			error = expand_number(argv[++i], &opts.virtual_size);
			if (error != 0) {
				error = errno != 0 ? errno : EINVAL;
				goto out;
			}
		} else if (argv[i][0] == '-') {
			error = EINVAL;
			goto out;
		} else if (opts.path == NULL) {
			opts.path = argv[i];
		} else if (opts.base_uri == NULL) {
			error = make_file_uri(argv[i], &base_uri);
			if (error != 0)
				goto out;
			opts.base_uri = base_uri;
		} else {
			error = EINVAL;
			goto out;
		}
	}
	if (format == NULL || strcmp(format, "sco") != 0) {
		error = EINVAL;
		goto out;
	}
	error = create_sco(&opts);
out:
	free(base_uri);
	return (error);
}

int
main(int argc, char **argv)
{
	int error;

	if (argc < 2) {
		usage();
		return (2);
	}
	if (strcmp(argv[1], "create") == 0) {
		error = cmd_create(argc, argv);
	} else if (strcmp(argv[1], "seal") == 0 && argc == 3) {
		error = cmd_set_sealed(argv[2], true);
	} else if (strcmp(argv[1], "unseal") == 0 && argc == 3) {
		error = cmd_set_sealed(argv[2], false);
	} else if (strcmp(argv[1], "info") == 0 && argc == 3) {
		error = cmd_info(argv[2]);
	} else if (strcmp(argv[1], "check") == 0 && argc == 3) {
		error = cmd_check(argv[2]);
	} else {
		usage();
		return (2);
	}
	if (error != 0) {
		fprintf(stderr, "scorpi-image: %s\n", strerror(error));
		return (1);
	}
	return (0);
}
