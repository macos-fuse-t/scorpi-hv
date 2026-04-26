/* Internal image backend registry coverage for snapshot support. */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "scorpi_image.h"

static int low_probe_calls;
static int high_probe_calls;

static int
low_probe(int fd, uint32_t *score)
{
	assert(fd == 7);
	low_probe_calls++;
	*score = 10;
	return (0);
}

static int
high_probe(int fd, uint32_t *score)
{
	assert(fd == 7);
	high_probe_calls++;
	*score = 42;
	return (0);
}

static int
noop_open(const char *path __attribute__((unused)),
    int fd __attribute__((unused)), bool readonly __attribute__((unused)),
    void **state __attribute__((unused)))
{
	return (0);
}

static int
noop_get_info(void *state __attribute__((unused)),
    struct scorpi_image_info *info __attribute__((unused)))
{
	return (0);
}

static int
noop_map(void *state __attribute__((unused)),
    uint64_t offset __attribute__((unused)),
    uint64_t length __attribute__((unused)),
    struct scorpi_image_extent *extent __attribute__((unused)))
{
	return (0);
}

static int
noop_read(void *state __attribute__((unused)),
    void *buf __attribute__((unused)), uint64_t offset __attribute__((unused)),
    size_t len __attribute__((unused)))
{
	return (0);
}

static int
noop_write(void *state __attribute__((unused)),
    const void *buf __attribute__((unused)),
    uint64_t offset __attribute__((unused)),
    size_t len __attribute__((unused)))
{
	return (0);
}

static int
noop_discard(void *state __attribute__((unused)),
    uint64_t offset __attribute__((unused)),
    uint64_t length __attribute__((unused)))
{
	return (0);
}

static int
noop_flush(void *state __attribute__((unused)))
{
	return (0);
}

static int
noop_close(void *state __attribute__((unused)))
{
	return (0);
}

static const struct scorpi_image_ops low_backend = {
	.name = "test-low",
	.probe = low_probe,
	.open = noop_open,
	.get_info = noop_get_info,
	.map = noop_map,
	.read = noop_read,
	.write = noop_write,
	.discard = noop_discard,
	.flush = noop_flush,
	.close = noop_close,
};

static const struct scorpi_image_ops high_backend = {
	.name = "test-high",
	.probe = high_probe,
	.open = noop_open,
	.get_info = noop_get_info,
	.map = noop_map,
	.read = noop_read,
	.write = noop_write,
	.discard = noop_discard,
	.flush = noop_flush,
	.close = noop_close,
};

int
main(void)
{
	const struct scorpi_image_ops *best;
	uint32_t score;

	assert(scorpi_image_backend_find("test-low") == NULL);
	assert(scorpi_image_backend_register(&low_backend) == 0);
	assert(scorpi_image_backend_register(&low_backend) != 0);
	assert(scorpi_image_backend_register(&high_backend) == 0);
	assert(scorpi_image_backend_find("test-low") == &low_backend);
	assert(scorpi_image_backend_find("test-high") == &high_backend);

	best = NULL;
	score = 0;
	assert(scorpi_image_backend_probe(7, &best, &score) == 0);
	assert(best == &high_backend);
	assert(score == 42);
	assert(low_probe_calls == 1);
	assert(high_probe_calls == 1);

	assert(scorpi_image_backend_unregister("test-low") == 0);
	assert(scorpi_image_backend_unregister("test-high") == 0);
	assert(scorpi_image_backend_find("test-low") == NULL);
	assert(scorpi_image_backend_find("test-high") == NULL);

	best = NULL;
	score = 99;
	assert(scorpi_image_backend_probe(7, &best, &score) != 0);
	assert(best == NULL);
	assert(score == 0);

	return (0);
}
