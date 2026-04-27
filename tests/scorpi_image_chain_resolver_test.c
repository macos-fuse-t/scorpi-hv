/* Backing-chain resolver coverage for snapshot support. */

#include <sys/errno.h>
#include <sys/param.h>
#include <sys/stat.h>

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "scorpi_image_chain.h"

#define	DIGEST_A \
	"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define	DIGEST_B \
	"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

struct base_test_state {
	int fd;
	bool readonly;
	bool sealed;
	uint64_t virtual_size;
	uint32_t logical_sector_size;
	uint8_t image_digest[32];
	bool has_image_digest;
	uint8_t base_digest[32];
	bool has_base_digest;
	char *base_uri;
	enum scorpi_image_extent_state map_state;
	uint8_t data_byte;
};

static int
base_test_probe(int fd, uint32_t *score)
{
	char magic[5];

	assert(fd >= 0);
	if (pread(fd, magic, sizeof(magic), 0) != (ssize_t)sizeof(magic)) {
		*score = 0;
		return (0);
	}
	*score = memcmp(magic, "PTST\n", sizeof(magic)) == 0 ? 100 : 0;
	return (0);
}

static uint64_t
parse_u64(const char *value)
{
	char *end;
	uint64_t number;

	errno = 0;
	number = strtoull(value, &end, 10);
	assert(errno == 0);
	assert(end != value && *end == '\0');
	return (number);
}

static uint32_t
parse_u32(const char *value)
{
	uint64_t number;

	number = parse_u64(value);
	assert(number <= UINT32_MAX);
	return ((uint32_t)number);
}

static uint8_t
parse_u8(const char *value)
{
	uint64_t number;

	number = parse_u64(value);
	assert(number <= UINT8_MAX);
	return ((uint8_t)number);
}

static bool
parse_bool(const char *value)
{
	if (strcmp(value, "true") == 0)
		return (true);
	assert(strcmp(value, "false") == 0);
	return (false);
}

static enum scorpi_image_extent_state
parse_map_state(const char *value)
{
	if (strcmp(value, "present") == 0)
		return (SCORPI_IMAGE_EXTENT_PRESENT);
	if (strcmp(value, "absent") == 0)
		return (SCORPI_IMAGE_EXTENT_ABSENT);
	if (strcmp(value, "zero") == 0)
		return (SCORPI_IMAGE_EXTENT_ZERO);
	assert(strcmp(value, "discarded") == 0);
	return (SCORPI_IMAGE_EXTENT_DISCARDED);
}

static uint8_t
hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return ((uint8_t)(c - '0'));
	if (c >= 'a' && c <= 'f')
		return ((uint8_t)(c - 'a' + 10));
	assert(c >= 'A' && c <= 'F');
	return ((uint8_t)(c - 'A' + 10));
}

static void
parse_digest(const char *value, uint8_t digest[32])
{
	size_t i;

	assert(strlen(value) == 64);
	for (i = 0; i < 32; i++)
		digest[i] = (uint8_t)((hex_value(value[i * 2]) << 4) |
		    hex_value(value[i * 2 + 1]));
}

static void
read_image_metadata(int fd, struct base_test_state *state)
{
	char buf[512];
	char *line, *next, *value, *end;
	ssize_t n;

	n = pread(fd, buf, sizeof(buf) - 1, 0);
	assert(n >= 0);
	buf[n] = '\0';

	line = buf;
	while (line != NULL && line[0] != '\0') {
		next = strchr(line, '\n');
		if (next != NULL)
			*next++ = '\0';
		if (strncmp(line, "base=", 5) == 0) {
			value = line + 5;
			end = value + strlen(value);
			while (end > value && (end[-1] == '\r' || end[-1] == ' '))
				*--end = '\0';
			state->base_uri = strdup(value);
			assert(state->base_uri != NULL);
		} else if (strncmp(line, "size=", 5) == 0) {
			state->virtual_size = parse_u64(line + 5);
		} else if (strncmp(line, "sector=", 7) == 0) {
			state->logical_sector_size = parse_u32(line + 7);
		} else if (strncmp(line, "readonly=", 9) == 0) {
			state->readonly = parse_bool(line + 9);
		} else if (strncmp(line, "sealed=", 7) == 0) {
			state->sealed = parse_bool(line + 7);
		} else if (strncmp(line, "digest=", 7) == 0) {
			parse_digest(line + 7, state->image_digest);
			state->has_image_digest = true;
		} else if (strncmp(line, "base_digest=", 12) == 0) {
			parse_digest(line + 12, state->base_digest);
			state->has_base_digest = true;
		} else if (strncmp(line, "map=", 4) == 0) {
			state->map_state = parse_map_state(line + 4);
		} else if (strncmp(line, "byte=", 5) == 0) {
			state->data_byte = parse_u8(line + 5);
		}
		line = next;
	}
}

static int
base_test_open(const char *path __attribute__((unused)), int fd,
    bool readonly, void **statep)
{
	struct base_test_state *state;

	state = calloc(1, sizeof(*state));
	if (state == NULL)
		return (ENOMEM);
	state->fd = fd;
	state->readonly = readonly;
	state->virtual_size = 128 * 1024 * 1024;
	state->logical_sector_size = 512;
	state->map_state = SCORPI_IMAGE_EXTENT_PRESENT;
	state->data_byte = 0x5a;
	read_image_metadata(fd, state);
	*statep = state;
	return (0);
}

static int
base_test_get_info(void *statep, struct scorpi_image_info *info)
{
	struct base_test_state *state;

	state = statep;
	memset(info, 0, sizeof(*info));
	info->format = SCORPI_IMAGE_FORMAT_SCO;
	info->virtual_size = state->virtual_size;
	info->logical_sector_size = state->logical_sector_size;
	info->physical_sector_size = 4096;
	info->cluster_size = 65536;
	info->readonly = state->readonly;
	info->sealed = state->sealed;
	if (state->has_image_digest) {
		memcpy(info->image_digest, state->image_digest,
		    sizeof(info->image_digest));
		info->has_image_digest = true;
	}
	info->has_base = state->base_uri != NULL;
	if (state->base_uri != NULL) {
		info->base_uri = strdup(state->base_uri);
		if (info->base_uri == NULL)
			return (ENOMEM);
	}
	if (state->has_base_digest) {
		memcpy(info->base_digest, state->base_digest,
		    sizeof(info->base_digest));
		info->has_base_digest = true;
	}
	return (0);
}

static int
base_test_map(void *statep, uint64_t offset, uint64_t length,
    struct scorpi_image_extent *extent)
{
	struct base_test_state *state;

	if (statep == NULL || extent == NULL)
		return (EINVAL);
	state = statep;
	if (offset > state->virtual_size)
		return (EINVAL);
	if (length > state->virtual_size - offset)
		length = state->virtual_size - offset;
	if (length == 0)
		return (EINVAL);

	*extent = (struct scorpi_image_extent){
		.offset = offset,
		.length = length,
		.state = state->map_state,
	};
	return (0);
}

static int
base_test_read(void *statep, void *buf, uint64_t offset, size_t len)
{
	struct base_test_state *state;

	if (statep == NULL || buf == NULL)
		return (EINVAL);
	state = statep;
	if (offset > state->virtual_size ||
	    (uint64_t)len > state->virtual_size - offset)
		return (EINVAL);
	memset(buf, state->data_byte, len);
	return (0);
}

static int
base_test_write(void *statep, const void *buf, uint64_t offset, size_t len)
{
	struct base_test_state *state;

	if (statep == NULL || buf == NULL)
		return (EINVAL);
	state = statep;
	if (state->readonly)
		return (EROFS);
	if (offset > state->virtual_size ||
	    (uint64_t)len > state->virtual_size - offset)
		return (EINVAL);
	if (len > 0)
		state->data_byte = ((const uint8_t *)buf)[0];
	state->map_state = SCORPI_IMAGE_EXTENT_PRESENT;
	return (0);
}

static int
base_test_discard(void *statep, uint64_t offset, uint64_t length)
{
	struct base_test_state *state;

	if (statep == NULL)
		return (EINVAL);
	state = statep;
	if (state->readonly)
		return (EROFS);
	if (offset > state->virtual_size || length > state->virtual_size - offset)
		return (EINVAL);
	state->map_state = SCORPI_IMAGE_EXTENT_DISCARDED;
	return (0);
}

static int
base_test_flush(void *state __attribute__((unused)))
{
	return (0);
}

static int
base_test_close(void *statep)
{
	struct base_test_state *state;

	state = statep;
	if (state == NULL)
		return (0);
	assert(close(state->fd) == 0);
	free(state->base_uri);
	free(state);
	return (0);
}

static const struct scorpi_image_ops base_test_backend = {
	.name = "test-base-chain",
	.probe = base_test_probe,
	.open = base_test_open,
	.get_info = base_test_get_info,
	.map = base_test_map,
	.read = base_test_read,
	.write = base_test_write,
	.discard = base_test_discard,
	.flush = base_test_flush,
	.close = base_test_close,
};
SCORPI_IMAGE_BACKEND_SET(base_test_backend);

static void
write_file(const char *path, const char *contents)
{
	int fd;

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	assert(fd >= 0);
	assert(write(fd, contents, strlen(contents)) == (ssize_t)strlen(contents));
	assert(close(fd) == 0);
}

static void
make_path(char *buf, size_t buflen, const char *dir, const char *name)
{
	assert(snprintf(buf, buflen, "%s/%s", dir, name) < (int)buflen);
}

static void
assert_open_error_with_readonly(const char *path, bool readonly,
    const struct scorpi_image_chain_open_options *options, int expected)
{
	struct scorpi_image_chain *chain;
	int fd;

	fd = open(path, readonly ? O_RDONLY : O_RDWR);
	assert(fd >= 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single(path, fd, readonly, options,
	    &chain) == expected);
	assert(chain == NULL);
}

static void
assert_open_error(const char *path,
    const struct scorpi_image_chain_open_options *options, int expected)
{
	assert_open_error_with_readonly(path, true, options, expected);
}

static struct scorpi_image_chain *
open_chain(const char *path, bool readonly)
{
	struct scorpi_image_chain_open_options options;
	struct scorpi_image_chain *chain;
	int fd;

	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = false,
	};
	fd = open(path, readonly ? O_RDONLY : O_RDWR);
	assert(fd >= 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single(path, fd, readonly, &options,
	    &chain) == 0);
	assert(chain != NULL);
	return (chain);
}

static void
assert_buffer_value(const uint8_t *buf, size_t len, uint8_t expected)
{
	size_t i;

	for (i = 0; i < len; i++)
		assert(buf[i] == expected);
}

static void
test_raw_depth_one(const char *dir)
{
	const struct scorpi_image_info *info;
	struct scorpi_image_chain_open_options options;
	struct scorpi_image_chain *chain;
	char path[MAXPATHLEN];
	int fd;

	make_path(path, sizeof(path), dir, "base.raw");
	write_file(path, "raw image");

	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = true,
	};
	fd = open(path, O_RDONLY);
	assert(fd >= 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single(path, fd, true, &options,
	    &chain) == 0);
	assert(scorpi_image_chain_layer_count(chain) == 1);
	info = scorpi_image_chain_layer_info(chain, 0);
	assert(info != NULL);
	assert(info->format == SCORPI_IMAGE_FORMAT_RAW);
	assert(scorpi_image_chain_close(chain) == 0);
}

static void
test_simple_base_chain(const char *dir)
{
	const struct scorpi_image_info *info;
	struct scorpi_image_chain_open_options options;
	struct scorpi_image_chain *chain;
	char child[MAXPATHLEN], base[MAXPATHLEN];
	int fd;

	make_path(child, sizeof(child), dir, "child.ptst");
	make_path(base, sizeof(base), dir, "base.ptst");
	write_file(base, "PTST\n");
	write_file(child, "PTST\nbase=file:base.ptst\n");

	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = false,
	};
	fd = open(child, O_RDONLY);
	assert(fd >= 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single(child, fd, false, &options,
	    &chain) == 0);
	assert(scorpi_image_chain_layer_count(chain) == 2);
	info = scorpi_image_chain_layer_info(chain, 0);
	assert(info != NULL);
	assert(info->format == SCORPI_IMAGE_FORMAT_SCO);
	assert(!info->readonly);
	info = scorpi_image_chain_layer_info(chain, 1);
	assert(info != NULL);
	assert(info->format == SCORPI_IMAGE_FORMAT_SCO);
	assert(info->readonly);
	assert(scorpi_image_chain_close(chain) == 0);
}

static void
test_missing_base(const char *dir)
{
	struct scorpi_image_chain_open_options options;
	char child[MAXPATHLEN];

	make_path(child, sizeof(child), dir, "base-missing-child.ptst");
	write_file(child, "PTST\nbase=file:missing.ptst\n");

	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = false,
	};
	assert_open_error(child, &options, ENOENT);
}

static void
test_cycle(const char *dir)
{
	struct scorpi_image_chain_open_options options;
	char a[MAXPATHLEN], b[MAXPATHLEN];

	make_path(a, sizeof(a), dir, "cycle-a.ptst");
	make_path(b, sizeof(b), dir, "cycle-b.ptst");
	write_file(a, "PTST\nbase=file:cycle-b.ptst\n");
	write_file(b, "PTST\nbase=file:cycle-a.ptst\n");

	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = false,
	};
	assert_open_error(a, &options, ELOOP);
}

static void
test_max_depth(const char *dir)
{
	struct scorpi_image_chain_open_options options;
	char a[MAXPATHLEN], b[MAXPATHLEN], c[MAXPATHLEN];

	make_path(a, sizeof(a), dir, "depth-a.ptst");
	make_path(b, sizeof(b), dir, "depth-b.ptst");
	make_path(c, sizeof(c), dir, "depth-c.ptst");
	write_file(a, "PTST\nbase=file:depth-b.ptst\n");
	write_file(b, "PTST\nbase=file:depth-c.ptst\n");
	write_file(c, "PTST\n");

	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = false,
		.max_depth = 2,
	};
	assert_open_error(a, &options, E2BIG);
}

static void
test_reject_size_mismatch(const char *dir)
{
	struct scorpi_image_chain_open_options options;
	char child[MAXPATHLEN], base[MAXPATHLEN];

	make_path(child, sizeof(child), dir, "size-child.ptst");
	make_path(base, sizeof(base), dir, "size-base.ptst");
	write_file(child, "PTST\nbase=file:size-base.ptst\nsize=134217728\n");
	write_file(base, "PTST\nsize=67108864\n");

	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = false,
	};
	assert_open_error(child, &options, EINVAL);
}

static void
test_reject_sector_mismatch(const char *dir)
{
	struct scorpi_image_chain_open_options options;
	char child[MAXPATHLEN], base[MAXPATHLEN];

	make_path(child, sizeof(child), dir, "sector-child.ptst");
	make_path(base, sizeof(base), dir, "sector-base.ptst");
	write_file(child, "PTST\nbase=file:sector-base.ptst\nsector=512\n");
	write_file(base, "PTST\nsector=4096\n");

	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = false,
	};
	assert_open_error(child, &options, EINVAL);
}

static void
test_reject_writable_base(const char *dir)
{
	struct scorpi_image_chain_open_options options;
	char child[MAXPATHLEN], base[MAXPATHLEN];

	make_path(child, sizeof(child), dir, "writable-base-child.ptst");
	make_path(base, sizeof(base), dir, "writable-base.ptst");
	write_file(child, "PTST\nbase=file:writable-base.ptst\n");
	write_file(base, "PTST\nreadonly=false\n");

	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = false,
	};
	assert_open_error(child, &options, EROFS);
}

static void
test_reject_readonly_top_for_writable_open(const char *dir)
{
	struct scorpi_image_chain_open_options options;
	char child[MAXPATHLEN];

	make_path(child, sizeof(child), dir, "readonly-top.ptst");
	write_file(child, "PTST\nreadonly=true\n");

	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = false,
	};
	assert_open_error_with_readonly(child, false, &options, EROFS);
}

static void
test_accept_matching_base_digest(const char *dir)
{
	struct scorpi_image_chain_open_options options;
	struct scorpi_image_chain *chain;
	char child[MAXPATHLEN], base[MAXPATHLEN];
	int fd;

	make_path(child, sizeof(child), dir, "digest-child.ptst");
	make_path(base, sizeof(base), dir, "digest-base.ptst");
	write_file(child, "PTST\nbase=file:digest-base.ptst\nbase_digest="
	    DIGEST_A "\n");
	write_file(base, "PTST\ndigest=" DIGEST_A "\n");

	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = false,
	};
	fd = open(child, O_RDONLY);
	assert(fd >= 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single(child, fd, true, &options,
	    &chain) == 0);
	assert(chain != NULL);
	assert(scorpi_image_chain_close(chain) == 0);
}

static void
test_reject_base_digest_mismatch(const char *dir)
{
	struct scorpi_image_chain_open_options options;
	char child[MAXPATHLEN], base[MAXPATHLEN];

	make_path(child, sizeof(child), dir, "digest-mismatch-child.ptst");
	make_path(base, sizeof(base), dir, "digest-mismatch-base.ptst");
	write_file(child, "PTST\nbase=file:digest-mismatch-base.ptst\n"
	    "base_digest=" DIGEST_A "\n");
	write_file(base, "PTST\ndigest=" DIGEST_B "\n");

	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = false,
	};
	assert_open_error(child, &options, EINVAL);
}

static void
test_read_from_top_layer(const char *dir)
{
	struct scorpi_image_chain *chain;
	char child[MAXPATHLEN], base[MAXPATHLEN];
	uint8_t buf[32];

	make_path(child, sizeof(child), dir, "read-top-child.ptst");
	make_path(base, sizeof(base), dir, "read-top-base.ptst");
	write_file(child, "PTST\nbase=file:read-top-base.ptst\nmap=present\n"
	    "byte=17\n");
	write_file(base, "PTST\nmap=present\nbyte=34\n");

	chain = open_chain(child, true);
	memset(buf, 0, sizeof(buf));
	assert(scorpi_image_chain_read(chain, buf, 4096, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 17);
	assert(scorpi_image_chain_close(chain) == 0);
}

static void
test_read_absent_falls_through_to_base(const char *dir)
{
	struct scorpi_image_chain *chain;
	char child[MAXPATHLEN], base[MAXPATHLEN];
	uint8_t buf[32];

	make_path(child, sizeof(child), dir, "read-base-child.ptst");
	make_path(base, sizeof(base), dir, "read-base.ptst");
	write_file(child, "PTST\nbase=file:read-base.ptst\nmap=absent\n"
	    "byte=17\n");
	write_file(base, "PTST\nmap=present\nbyte=34\n");

	chain = open_chain(child, true);
	memset(buf, 0, sizeof(buf));
	assert(scorpi_image_chain_read(chain, buf, 8192, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 34);
	assert(scorpi_image_chain_close(chain) == 0);
}

static void
test_read_absent_without_base_returns_zero(const char *dir)
{
	struct scorpi_image_chain *chain;
	char child[MAXPATHLEN];
	uint8_t buf[32];

	make_path(child, sizeof(child), dir, "read-absent-zero.ptst");
	write_file(child, "PTST\nmap=absent\nbyte=17\n");

	chain = open_chain(child, true);
	memset(buf, 0xff, sizeof(buf));
	assert(scorpi_image_chain_read(chain, buf, 12288, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 0);
	assert(scorpi_image_chain_close(chain) == 0);
}

static void
test_read_zero_and_discarded_stop_base_fallthrough(const char *dir)
{
	struct scorpi_image_chain *chain;
	char zero_child[MAXPATHLEN], discarded_child[MAXPATHLEN], base[MAXPATHLEN];
	uint8_t buf[32];

	make_path(base, sizeof(base), dir, "read-zero-base.ptst");
	make_path(zero_child, sizeof(zero_child), dir, "read-zero-child.ptst");
	make_path(discarded_child, sizeof(discarded_child), dir,
	    "read-discarded-child.ptst");
	write_file(base, "PTST\nmap=present\nbyte=34\n");
	write_file(zero_child, "PTST\nbase=file:read-zero-base.ptst\nmap=zero\n"
	    "byte=17\n");
	write_file(discarded_child, "PTST\nbase=file:read-zero-base.ptst\n"
	    "map=discarded\nbyte=17\n");

	chain = open_chain(zero_child, true);
	memset(buf, 0xff, sizeof(buf));
	assert(scorpi_image_chain_read(chain, buf, 16384, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 0);
	assert(scorpi_image_chain_close(chain) == 0);

	chain = open_chain(discarded_child, true);
	memset(buf, 0xff, sizeof(buf));
	assert(scorpi_image_chain_read(chain, buf, 20480, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 0);
	assert(scorpi_image_chain_close(chain) == 0);
}

static void
test_read_cache_invalidated_by_write(const char *dir)
{
	struct scorpi_image_chain *chain;
	char child[MAXPATHLEN], base[MAXPATHLEN];
	uint8_t buf[32], writebuf[32];

	make_path(child, sizeof(child), dir, "write-cache-child.ptst");
	make_path(base, sizeof(base), dir, "write-cache-base.ptst");
	write_file(child, "PTST\nbase=file:write-cache-base.ptst\nmap=absent\n"
	    "byte=17\n");
	write_file(base, "PTST\nmap=present\nbyte=34\n");

	chain = open_chain(child, false);
	memset(buf, 0, sizeof(buf));
	assert(scorpi_image_chain_read(chain, buf, 24576, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 34);
	memset(writebuf, 51, sizeof(writebuf));
	assert(scorpi_image_chain_write(chain, writebuf, 24576,
	    sizeof(writebuf)) == 0);
	memset(buf, 0, sizeof(buf));
	assert(scorpi_image_chain_read(chain, buf, 24576, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 51);
	assert(scorpi_image_chain_close(chain) == 0);
}

static void
test_read_cache_invalidated_by_discard(const char *dir)
{
	struct scorpi_image_chain *chain;
	char child[MAXPATHLEN];
	uint8_t buf[32];

	make_path(child, sizeof(child), dir, "discard-cache-child.ptst");
	write_file(child, "PTST\nmap=present\nbyte=17\n");

	chain = open_chain(child, false);
	memset(buf, 0, sizeof(buf));
	assert(scorpi_image_chain_read(chain, buf, 28672, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 17);
	assert(scorpi_image_chain_discard(chain, 28672, sizeof(buf)) == 0);
	memset(buf, 0xff, sizeof(buf));
	assert(scorpi_image_chain_read(chain, buf, 28672, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 0);
	assert(scorpi_image_chain_close(chain) == 0);
}

int
main(void)
{
	char dir[] = "/tmp/scorpi-chain-resolver-XXXXXX";

	assert(mkdtemp(dir) != NULL);

	test_raw_depth_one(dir);
	test_simple_base_chain(dir);
	test_missing_base(dir);
	test_cycle(dir);
	test_max_depth(dir);
	test_reject_size_mismatch(dir);
	test_reject_sector_mismatch(dir);
	test_reject_writable_base(dir);
	test_reject_readonly_top_for_writable_open(dir);
	test_accept_matching_base_digest(dir);
	test_reject_base_digest_mismatch(dir);
	test_read_from_top_layer(dir);
	test_read_absent_falls_through_to_base(dir);
	test_read_absent_without_base_returns_zero(dir);
	test_read_zero_and_discarded_stop_base_fallthrough(dir);
	test_read_cache_invalidated_by_write(dir);
	test_read_cache_invalidated_by_discard(dir);

	return (0);
}
