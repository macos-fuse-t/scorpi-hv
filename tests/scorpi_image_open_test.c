/* Image-chain open probing and raw fallback coverage for snapshot support. */

#include <sys/errno.h>

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "scorpi_image_chain.h"

static int magic_open_calls;
static int magic_close_calls;

static int
magic_probe(int fd, uint32_t *score)
{
	char magic[4];

	assert(fd >= 0);
	assert(pread(fd, magic, sizeof(magic), 0) == (ssize_t)sizeof(magic));
	if (memcmp(magic, "SCO!", sizeof(magic)) == 0)
		*score = 100;
	else
		*score = 0;
	return (0);
}

static int
magic_open(const char *path __attribute__((unused)),
    int fd __attribute__((unused)), bool readonly __attribute__((unused)),
    void **state)
{
	magic_open_calls++;
	*state = (void *)&magic_open_calls;
	return (0);
}

static int
magic_get_info(void *state __attribute__((unused)),
    struct scorpi_image_info *info __attribute__((unused)))
{
	return (0);
}

static int
magic_map(void *state __attribute__((unused)),
    uint64_t offset __attribute__((unused)),
    uint64_t length __attribute__((unused)),
    struct scorpi_image_extent *extent __attribute__((unused)))
{
	return (0);
}

static int
magic_read(void *state __attribute__((unused)),
    void *buf __attribute__((unused)), uint64_t offset __attribute__((unused)),
    size_t len __attribute__((unused)))
{
	return (0);
}

static int
magic_write(void *state __attribute__((unused)),
    const void *buf __attribute__((unused)),
    uint64_t offset __attribute__((unused)),
    size_t len __attribute__((unused)))
{
	return (0);
}

static int
magic_discard(void *state __attribute__((unused)),
    uint64_t offset __attribute__((unused)),
    uint64_t length __attribute__((unused)))
{
	return (0);
}

static int
magic_flush(void *state __attribute__((unused)))
{
	return (0);
}

static int
magic_close(void *state __attribute__((unused)))
{
	magic_close_calls++;
	return (0);
}

static const struct scorpi_image_ops magic_backend = {
	.name = "test-magic-open",
	.probe = magic_probe,
	.open = magic_open,
	.get_info = magic_get_info,
	.map = magic_map,
	.read = magic_read,
	.write = magic_write,
	.discard = magic_discard,
	.flush = magic_flush,
	.close = magic_close,
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

int
main(void)
{
	const struct scorpi_image_info *info;
	struct scorpi_image_chain *chain;
	struct scorpi_image_chain_open_options options;
	char magic_path[] = "/tmp/scorpi-open-magic-XXXXXX";
	char raw_path[] = "/tmp/scorpi-open-raw-XXXXXX";
	int fd;

	fd = mkstemp(magic_path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	fd = mkstemp(raw_path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	write_file(magic_path, "SCO!payload");
	write_file(raw_path, "plain raw payload");

	assert(scorpi_image_backend_register(&magic_backend) == 0);

	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = true,
	};
	fd = open(magic_path, O_RDONLY);
	assert(fd >= 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single(magic_path, fd, true, &options,
	    &chain) == 0);
	assert(magic_open_calls == 1);
	assert(scorpi_image_chain_close(chain) == 0);
	assert(magic_close_calls == 1);

	fd = open(raw_path, O_RDONLY);
	assert(fd >= 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single(raw_path, fd, true, &options,
	    &chain) == 0);
	info = scorpi_image_chain_top_info(chain);
	assert(info != NULL);
	assert(info->format == SCORPI_IMAGE_FORMAT_RAW);
	assert(scorpi_image_chain_close(chain) == 0);

	options.raw_fallback = false;
	fd = open(raw_path, O_RDONLY);
	assert(fd >= 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single(raw_path, fd, true, &options,
	    &chain) == ENOENT);
	assert(chain == NULL);
	assert(close(fd) == 0);

	assert(scorpi_image_backend_unregister("test-magic-open") == 0);
	assert(unlink(magic_path) == 0);
	assert(unlink(raw_path) == 0);
	return (0);
}
