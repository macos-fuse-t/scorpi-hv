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

struct parent_test_state {
	int fd;
	bool readonly;
	char *parent_uri;
};

static int
parent_test_probe(int fd, uint32_t *score)
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

static char *
read_parent_uri(int fd)
{
	char buf[512];
	char *line, *next, *value, *end, *parent;
	ssize_t n;

	n = pread(fd, buf, sizeof(buf) - 1, 0);
	assert(n >= 0);
	buf[n] = '\0';

	line = buf;
	while (line != NULL && line[0] != '\0') {
		next = strchr(line, '\n');
		if (next != NULL)
			*next++ = '\0';
		if (strncmp(line, "parent=", 7) == 0) {
			value = line + 7;
			end = value + strlen(value);
			while (end > value && (end[-1] == '\r' || end[-1] == ' '))
				*--end = '\0';
			parent = strdup(value);
			assert(parent != NULL);
			return (parent);
		}
		line = next;
	}
	return (NULL);
}

static int
parent_test_open(const char *path __attribute__((unused)), int fd,
    bool readonly, void **statep)
{
	struct parent_test_state *state;

	state = calloc(1, sizeof(*state));
	if (state == NULL)
		return (ENOMEM);
	state->fd = fd;
	state->readonly = readonly;
	state->parent_uri = read_parent_uri(fd);
	*statep = state;
	return (0);
}

static int
parent_test_get_info(void *statep, struct scorpi_image_info *info)
{
	struct parent_test_state *state;

	state = statep;
	memset(info, 0, sizeof(*info));
	info->format = SCORPI_IMAGE_FORMAT_SCO;
	info->virtual_size = 128 * 1024 * 1024;
	info->logical_sector_size = 512;
	info->physical_sector_size = 4096;
	info->cluster_size = 65536;
	info->readonly = state->readonly;
	info->has_parent = state->parent_uri != NULL;
	if (state->parent_uri != NULL) {
		info->parent_uri = strdup(state->parent_uri);
		if (info->parent_uri == NULL)
			return (ENOMEM);
	}
	return (0);
}

static int
parent_test_map(void *state __attribute__((unused)),
    uint64_t offset __attribute__((unused)),
    uint64_t length __attribute__((unused)),
    struct scorpi_image_extent *extent __attribute__((unused)))
{
	return (ENOTSUP);
}

static int
parent_test_read(void *state __attribute__((unused)),
    void *buf __attribute__((unused)), uint64_t offset __attribute__((unused)),
    size_t len __attribute__((unused)))
{
	return (ENOTSUP);
}

static int
parent_test_write(void *state __attribute__((unused)),
    const void *buf __attribute__((unused)),
    uint64_t offset __attribute__((unused)),
    size_t len __attribute__((unused)))
{
	return (ENOTSUP);
}

static int
parent_test_discard(void *state __attribute__((unused)),
    uint64_t offset __attribute__((unused)),
    uint64_t length __attribute__((unused)))
{
	return (ENOTSUP);
}

static int
parent_test_flush(void *state __attribute__((unused)))
{
	return (0);
}

static int
parent_test_close(void *statep)
{
	struct parent_test_state *state;

	state = statep;
	if (state == NULL)
		return (0);
	assert(close(state->fd) == 0);
	free(state->parent_uri);
	free(state);
	return (0);
}

static const struct scorpi_image_ops parent_test_backend = {
	.name = "test-parent-chain",
	.probe = parent_test_probe,
	.open = parent_test_open,
	.get_info = parent_test_get_info,
	.map = parent_test_map,
	.read = parent_test_read,
	.write = parent_test_write,
	.discard = parent_test_discard,
	.flush = parent_test_flush,
	.close = parent_test_close,
};

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
assert_open_error(const char *path,
    const struct scorpi_image_chain_open_options *options, int expected)
{
	struct scorpi_image_chain *chain;
	int fd;

	fd = open(path, O_RDONLY);
	assert(fd >= 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single(path, fd, true, options,
	    &chain) == expected);
	assert(chain == NULL);
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
test_simple_parent_chain(const char *dir)
{
	const struct scorpi_image_info *info;
	struct scorpi_image_chain_open_options options;
	struct scorpi_image_chain *chain;
	char child[MAXPATHLEN], parent[MAXPATHLEN];
	int fd;

	make_path(child, sizeof(child), dir, "child.ptst");
	make_path(parent, sizeof(parent), dir, "parent.ptst");
	write_file(parent, "PTST\n");
	write_file(child, "PTST\nparent=file:parent.ptst\n");

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
test_missing_parent(const char *dir)
{
	struct scorpi_image_chain_open_options options;
	char child[MAXPATHLEN];

	make_path(child, sizeof(child), dir, "missing-child.ptst");
	write_file(child, "PTST\nparent=file:missing.ptst\n");

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
	write_file(a, "PTST\nparent=file:cycle-b.ptst\n");
	write_file(b, "PTST\nparent=file:cycle-a.ptst\n");

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
	write_file(a, "PTST\nparent=file:depth-b.ptst\n");
	write_file(b, "PTST\nparent=file:depth-c.ptst\n");
	write_file(c, "PTST\n");

	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = false,
		.max_depth = 2,
	};
	assert_open_error(a, &options, E2BIG);
}

int
main(void)
{
	char dir[] = "/tmp/scorpi-chain-resolver-XXXXXX";

	assert(mkdtemp(dir) != NULL);
	assert(scorpi_image_backend_register(&parent_test_backend) == 0);

	test_raw_depth_one(dir);
	test_simple_parent_chain(dir);
	test_missing_parent(dir);
	test_cycle(dir);
	test_max_depth(dir);

	assert(scorpi_image_backend_unregister("test-parent-chain") == 0);
	return (0);
}
