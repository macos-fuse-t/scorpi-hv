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

#include "scorpi_crc32c.h"
#include "scorpi_image.h"
#include "scorpi_image_chain.h"
#include "scorpi_image_raw.h"
#include "scorpi_image_sco.h"
#include "scorpi_image_sco_format.h"
#include "scorpi_image_uri.h"

#define	SCO_DEFAULT_CLUSTER_SIZE		0x40000U

struct sco_create_options {
	const char *path;
	const char *base_uri;
	uint64_t virtual_size;
	bool has_explicit_size;
	uint32_t cluster_size;
	uint32_t logical_sector_size;
	uint32_t physical_sector_size;
};

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
sco_choose_cluster_size(struct sco_create_options *opts,
    uint64_t *cluster_countp, uint32_t *table_lengthp)
{
	uint64_t cluster_count, table_length;

	if (opts == NULL || cluster_countp == NULL || table_lengthp == NULL ||
	    opts->virtual_size == 0 || opts->logical_sector_size == 0)
		return (EINVAL);
	opts->cluster_size = SCO_DEFAULT_CLUSTER_SIZE;
	while (opts->cluster_size < opts->logical_sector_size)
		opts->cluster_size <<= 1;
	if (opts->cluster_size < SCO_MIN_CLUSTER_SIZE)
		opts->cluster_size = SCO_MIN_CLUSTER_SIZE;
	for (;;) {
		if (opts->cluster_size > SCO_MAX_CLUSTER_SIZE)
			return (EFBIG);
		if ((opts->cluster_size % opts->logical_sector_size) != 0)
			return (EINVAL);
		cluster_count = (opts->virtual_size + opts->cluster_size - 1) /
		    opts->cluster_size;
		if (cluster_count > UINT64_MAX / SCO_TABLE_SLOT_SIZE)
			return (EFBIG);
		table_length = align_up_u64(cluster_count *
		    SCO_TABLE_SLOT_SIZE, SCO_METADATA_PAGE_SIZE);
		if (table_length <= SCO_MAX_SINGLE_TABLE_BYTES) {
			*cluster_countp = cluster_count;
			*table_lengthp = (uint32_t)table_length;
			return (0);
		}
		opts->cluster_size <<= 1;
	}
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
	le16enc(buf + 0x0008, SCO_FORMAT_MAJOR);
	le32enc(buf + 0x000c, SCO_FILE_ID_SIZE);
	memcpy(buf + 0x0010, uuid, 16);
	le32enc(buf + 0x0020, SCO_CHECKSUM_CRC32C);
	crc = scorpi_crc32c(buf, SCO_FILE_ID_SIZE);
	le32enc(buf + 0x0024, crc);
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
	crc = scorpi_crc32c(buf, size);
	le32enc(buf + 0x0008, crc);
}

static void
build_superblock(uint8_t buf[SCO_SUPERBLOCK_SIZE],
    const struct sco_create_options *opts, const uint8_t uuid[16],
    uint64_t cluster_count, uint64_t data_area_offset,
    uint64_t base_descriptor_offset, uint32_t base_descriptor_length,
    uint64_t table_offset, uint32_t table_length)
{
	uint32_t crc;

	memset(buf, 0, SCO_SUPERBLOCK_SIZE);
	memcpy(buf, SCO_MAGIC, 8);
	le16enc(buf + 0x0008, SCO_FORMAT_MAJOR);
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
	le64enc(buf + 0x0060, table_offset);
	le32enc(buf + 0x0068, table_length);
	le32enc(buf + 0x006c, SCO_TABLE_SLOT_SIZE);
	le32enc(buf + 0x0070, SCO_INCOMPAT_FIXED_TABLE_V2);
	memcpy(buf + 0x0080, uuid, 16);
	crc = scorpi_crc32c(buf, SCO_SUPERBLOCK_SIZE);
	le32enc(buf + 0x0014, crc);
}

static int
create_sco(const struct sco_create_options *opts)
{
	uint8_t page[SCO_METADATA_PAGE_SIZE], uuid[16];
	uint8_t *base_descriptor;
	struct sco_create_options actual_opts;
	uint64_t cluster_count;
	uint64_t base_descriptor_offset, data_area_offset, metadata_end;
	uint64_t table_offset;
	uint32_t base_descriptor_length, table_length;
	int fd, error;

	if (opts->path == NULL || opts->virtual_size == 0 ||
	    opts->logical_sector_size == 0)
		return (EINVAL);
	actual_opts = *opts;
	if (!is_power_of_two_u64(actual_opts.logical_sector_size))
		return (EINVAL);
	if (actual_opts.physical_sector_size != 0 &&
	    (!is_power_of_two_u64(actual_opts.physical_sector_size) ||
	    actual_opts.physical_sector_size < actual_opts.logical_sector_size))
		return (EINVAL);

	error = sco_choose_cluster_size(&actual_opts, &cluster_count,
	    &table_length);
	if (error != 0)
		return (error);
	metadata_end = SCO_METADATA_AREA_OFFSET;
	base_descriptor_offset = 0;
	base_descriptor_length = 0;
	if (actual_opts.base_uri != NULL) {
		base_descriptor_offset = metadata_end;
		base_descriptor_length = (uint32_t)align_up_u64(0x50 +
		    strlen(actual_opts.base_uri), SCO_METADATA_PAGE_SIZE);
		if (base_descriptor_length > UINT64_MAX - metadata_end)
			return (EFBIG);
		metadata_end += base_descriptor_length;
	}
	table_offset = metadata_end;
	if (table_length > (UINT64_MAX - metadata_end) /
	    SCO_TABLE_SLOTS_PER_ENTRY)
		return (EFBIG);
	metadata_end += (uint64_t)table_length * SCO_TABLE_SLOTS_PER_ENTRY;
	if (metadata_end > UINT64_MAX - actual_opts.cluster_size + 1)
		return (EFBIG);
	data_area_offset = align_up_u64(metadata_end, actual_opts.cluster_size);

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
	build_superblock(page, &actual_opts, uuid, cluster_count, data_area_offset,
	    base_descriptor_offset, base_descriptor_length,
	    table_offset, table_length);
	error = write_exact(fd, SCO_SUPERBLOCK_A_OFFSET, page, sizeof(page));
	if (error != 0)
		goto out;
	memset(page, 0, sizeof(page));
	error = write_exact(fd, SCO_SUPERBLOCK_B_OFFSET, page, sizeof(page));
	if (error != 0)
		goto out;

	if (actual_opts.base_uri != NULL) {
		base_descriptor = calloc(1, base_descriptor_length);
		if (base_descriptor == NULL) {
			error = ENOMEM;
			goto out;
		}
		build_base_descriptor(base_descriptor, base_descriptor_length,
		    actual_opts.base_uri);
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
	if (fsync(fd) != 0) {
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
create_raw(const char *path, uint64_t virtual_size)
{
	int fd, error;

	if (path == NULL || virtual_size == 0 || virtual_size > INT64_MAX)
		return (EINVAL);
	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	if (fd < 0)
		return (errno);
	error = 0;
	if (ftruncate(fd, (off_t)virtual_size) != 0)
		error = errno;
	if (error == 0 && fsync(fd) != 0)
		error = errno;
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
open_image_for_create_base(const char *path,
    const struct scorpi_image_ops **opsp, void **statep)
{
	const struct scorpi_image_ops *ops;
	uint32_t score;
	int fd, error;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return (errno);
	ops = NULL;
	error = scorpi_image_backend_probe(fd, &ops, &score);
	if (error != 0 && error != ENOENT) {
		close(fd);
		return (error);
	}
	if (error == ENOENT || ops == NULL || score == 0)
		ops = scorpi_image_raw_backend();
	error = ops->open(path, fd, true, statep);
	if (error != 0) {
		close(fd);
		return (error);
	}
	*opsp = ops;
	return (0);
}

static int
read_base_create_info(const char *image_path, const char *base_uri,
    struct scorpi_image_info *info)
{
	struct scorpi_image_base_location *location;
	const struct scorpi_image_ops *ops;
	void *state;
	int error, close_error;

	if (image_path == NULL || base_uri == NULL || info == NULL)
		return (EINVAL);

	location = NULL;
	error = scorpi_image_base_location_resolve(image_path, base_uri, NULL,
	    &location);
	if (error != 0)
		return (error);

	state = NULL;
	memset(info, 0, sizeof(*info));
	error = open_image_for_create_base(location->resolved_path, &ops,
	    &state);
	if (error != 0) {
		scorpi_image_base_location_free(location);
		return (error);
	}
	error = ops->get_info(state, info);
	close_error = ops->close(state);
	if (error == 0)
		error = close_error;
	scorpi_image_base_location_free(location);
	return (error);
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
	struct scorpi_image_chain_open_options options;
	struct scorpi_image_chain_diagnostics diagnostics;
	struct scorpi_image_chain_layer_diagnostic *layer;
	struct scorpi_image_chain *chain;
	const struct scorpi_image_info *info;
	int error;
	size_t i;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return (errno);
	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = true,
	};
	chain = NULL;
	error = scorpi_image_chain_open_single(path, fd, true, &options,
	    &chain);
	if (error != 0)
		return (error);
	info = scorpi_image_chain_top_info(chain);
	if (info == NULL) {
		scorpi_image_chain_close(chain);
		return (EINVAL);
	}
	printf("format=%s\n", format_name(info->format));
	printf("virtual_size=%llu\n",
	    (unsigned long long)info->virtual_size);
	printf("logical_sector_size=%u\n", info->logical_sector_size);
	printf("physical_sector_size=%u\n", info->physical_sector_size);
	printf("cluster_size=%u\n", info->cluster_size);
	printf("readonly=%s\n", info->readonly ? "true" : "false");
	printf("sealed=%s\n", info->sealed ? "true" : "false");
	if (info->has_base)
		printf("base_uri=%s\n", info->base_uri);

	memset(&diagnostics, 0, sizeof(diagnostics));
	error = scorpi_image_chain_get_diagnostics(chain, &diagnostics);
	if (error == 0) {
		printf("chain_layers=%zu\n", diagnostics.layer_count);
		for (i = 0; i < diagnostics.layer_count; i++) {
			layer = &diagnostics.layers[i];
			printf("layer.%zu.format=%s\n", i, layer->format_name);
			printf("layer.%zu.virtual_size=%llu\n", i,
			    (unsigned long long)layer->virtual_size);
			printf("layer.%zu.logical_sector_size=%u\n", i,
			    layer->logical_sector_size);
			printf("layer.%zu.physical_sector_size=%u\n", i,
			    layer->physical_sector_size);
			printf("layer.%zu.cluster_size=%u\n", i,
			    layer->cluster_size);
			printf("layer.%zu.readonly=%s\n", i,
			    layer->readonly ? "true" : "false");
			printf("layer.%zu.sealed=%s\n", i,
			    layer->sealed ? "true" : "false");
			if (layer->source_uri != NULL)
				printf("layer.%zu.source_uri=%s\n", i,
				    layer->source_uri);
			if (layer->resolved_path != NULL)
				printf("layer.%zu.resolved_path=%s\n", i,
				    layer->resolved_path);
			if (layer->has_base && layer->base_uri != NULL)
				printf("layer.%zu.base_uri=%s\n", i,
				    layer->base_uri);
		}
	}
	scorpi_image_chain_diagnostics_free(&diagnostics);
	if (scorpi_image_chain_close(chain) != 0 && error == 0)
		error = EIO;
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
	    "  scorpi-image create --format sco [--size bytes|Nmb|Ngb] path [base]\n"
	    "  scorpi-image create --format raw --size bytes|Nmb|Ngb path\n"
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
	struct scorpi_image_info base_info;
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
			opts.has_explicit_size = true;
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
	if (format == NULL) {
		error = EINVAL;
		goto out;
	}
	if (strcmp(format, "raw") == 0) {
		if (!opts.has_explicit_size || opts.base_uri != NULL ||
		    opts.path == NULL) {
			error = EINVAL;
			goto out;
		}
		error = create_raw(opts.path, opts.virtual_size);
		goto out;
	}
	if (strcmp(format, "sco") != 0) {
		error = EINVAL;
		goto out;
	}
	if (opts.base_uri != NULL) {
		memset(&base_info, 0, sizeof(base_info));
		error = read_base_create_info(opts.path, opts.base_uri,
		    &base_info);
		if (error != 0)
			goto out;
		if (opts.has_explicit_size &&
		    opts.virtual_size != base_info.virtual_size) {
			free(base_info.base_uri);
			error = EINVAL;
			goto out;
		}
		opts.virtual_size = base_info.virtual_size;
		opts.logical_sector_size = base_info.logical_sector_size;
		if (base_info.physical_sector_size != 0)
			opts.physical_sector_size = base_info.physical_sector_size;
		free(base_info.base_uri);
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
