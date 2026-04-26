/* One-layer image chain coverage for snapshot support. */

#include <sys/errno.h>

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "scorpi_image_chain.h"
#include "scorpi_image_raw.h"

enum {
	CONCURRENT_CHAIN_BUFSIZE = 4096,
	CONCURRENT_CHAIN_READERS = 4,
	CONCURRENT_CHAIN_ITERATIONS = 200,
};

struct concurrent_chain_reader {
	struct scorpi_image_chain *chain;
	int error;
};

struct concurrent_chain_writer {
	struct scorpi_image_chain *chain;
	int error;
};

static void
write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p;
	ssize_t n;

	p = buf;
	while (len > 0) {
		n = write(fd, p, len);
		assert(n > 0);
		p += n;
		len -= (size_t)n;
	}
}

static bool
buffer_is_single_expected_value(const uint8_t *buf, size_t len)
{
	size_t i;

	if (len == 0)
		return (true);
	if (buf[0] != 0x11 && buf[0] != 0x22 && buf[0] != 0x33)
		return (false);
	for (i = 1; i < len; i++) {
		if (buf[i] != buf[0])
			return (false);
	}
	return (true);
}

static void *
concurrent_chain_reader_thread(void *argp)
{
	struct concurrent_chain_reader *arg;
	uint8_t buf[CONCURRENT_CHAIN_BUFSIZE];
	int i, error;

	arg = argp;
	for (i = 0; i < CONCURRENT_CHAIN_ITERATIONS; i++) {
		memset(buf, 0, sizeof(buf));
		error = scorpi_image_chain_read(arg->chain, buf, 0,
		    sizeof(buf));
		if (error != 0 || !buffer_is_single_expected_value(buf,
		    sizeof(buf))) {
			arg->error = error != 0 ? error : EIO;
			return (NULL);
		}
	}
	arg->error = 0;
	return (NULL);
}

static void *
concurrent_chain_writer_thread(void *argp)
{
	struct concurrent_chain_writer *arg;
	uint8_t buf[CONCURRENT_CHAIN_BUFSIZE];
	int i, error;

	arg = argp;
	for (i = 0; i < CONCURRENT_CHAIN_ITERATIONS; i++) {
		memset(buf, (i & 1) == 0 ? 0x22 : 0x33, sizeof(buf));
		error = scorpi_image_chain_write(arg->chain, buf, 0,
		    sizeof(buf));
		if (error != 0) {
			arg->error = error;
			return (NULL);
		}
	}
	arg->error = 0;
	return (NULL);
}

static void
test_concurrent_chain_read_write_serializes_cache(void)
{
	const struct scorpi_image_ops *raw;
	struct concurrent_chain_reader readers[CONCURRENT_CHAIN_READERS];
	struct concurrent_chain_writer writer;
	struct scorpi_image_chain *chain;
	pthread_t reader_threads[CONCURRENT_CHAIN_READERS], writer_thread;
	char path[] = "/tmp/scorpi-chain-thread-test-XXXXXX";
	uint8_t buf[CONCURRENT_CHAIN_BUFSIZE];
	void *state;
	int fd, i;

	fd = mkstemp(path);
	assert(fd >= 0);
	memset(buf, 0x11, sizeof(buf));
	write_all(fd, buf, sizeof(buf));
	close(fd);

	raw = scorpi_image_raw_backend();
	fd = open(path, O_RDWR);
	assert(fd >= 0);
	state = NULL;
	assert(raw->open(path, fd, false, &state) == 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single_backend(raw, state, &chain) == 0);

	writer = (struct concurrent_chain_writer){
		.chain = chain,
	};
	assert(pthread_create(&writer_thread, NULL,
	    concurrent_chain_writer_thread, &writer) == 0);
	for (i = 0; i < CONCURRENT_CHAIN_READERS; i++) {
		readers[i] = (struct concurrent_chain_reader){
			.chain = chain,
		};
		assert(pthread_create(&reader_threads[i], NULL,
		    concurrent_chain_reader_thread, &readers[i]) == 0);
	}
	assert(pthread_join(writer_thread, NULL) == 0);
	assert(writer.error == 0);
	for (i = 0; i < CONCURRENT_CHAIN_READERS; i++) {
		assert(pthread_join(reader_threads[i], NULL) == 0);
		assert(readers[i].error == 0);
	}
	assert(scorpi_image_chain_close(chain) == 0);
	assert(unlink(path) == 0);
}

static void
test_raw_chain_diagnostics(void)
{
	struct scorpi_image_chain_diagnostics diagnostics;
	struct scorpi_image_chain *chain;
	struct scorpi_image_chain_open_options options;
	char path[] = "/tmp/scorpi-chain-diag-XXXXXX";
	uint8_t buf[32];
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	memset(buf, 0x5a, sizeof(buf));
	write_all(fd, buf, sizeof(buf));
	assert(close(fd) == 0);

	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = true,
	};
	fd = open(path, O_RDONLY);
	assert(fd >= 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single(path, fd, true, &options,
	    &chain) == 0);
	memset(&diagnostics, 0, sizeof(diagnostics));
	assert(scorpi_image_chain_get_diagnostics(chain, &diagnostics) == 0);
	assert(diagnostics.layer_count == 1);
	assert(diagnostics.layers[0].index == 0);
	assert(diagnostics.layers[0].chain_depth == 1);
	assert(diagnostics.layers[0].format == SCORPI_IMAGE_FORMAT_RAW);
	assert(strcmp(diagnostics.layers[0].format_name, "raw") == 0);
	assert(diagnostics.layers[0].readonly);
	assert(diagnostics.layers[0].virtual_size == sizeof(buf));
	assert(diagnostics.layers[0].logical_sector_size == 512);
	assert(!diagnostics.layers[0].has_base);
	assert(strcmp(diagnostics.layers[0].source_uri, path) == 0);
	assert(strcmp(diagnostics.layers[0].resolved_path, path) == 0);
	scorpi_image_chain_diagnostics_free(&diagnostics);
	assert(diagnostics.layer_count == 0);
	assert(scorpi_image_chain_close(chain) == 0);
	assert(unlink(path) == 0);
}

int
main(void)
{
	const struct scorpi_image_info *info;
	const struct scorpi_image_ops *raw;
	struct scorpi_image_chain *chain;
	char path[] = "/tmp/scorpi-chain-test-XXXXXX";
	uint8_t buf[32], out[32];
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	memset(buf, 0x11, sizeof(buf));
	write_all(fd, buf, sizeof(buf));
	close(fd);

	raw = scorpi_image_raw_backend();
	fd = open(path, O_RDWR);
	assert(fd >= 0);
	state = NULL;
	assert(raw->open(path, fd, false, &state) == 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single_backend(raw, state, &chain) == 0);

	info = scorpi_image_chain_top_info(chain);
	assert(info != NULL);
	assert(info->format == SCORPI_IMAGE_FORMAT_RAW);
	assert(info->virtual_size == sizeof(buf));
	assert(!info->readonly);

	memset(out, 0, sizeof(out));
	assert(scorpi_image_chain_read(chain, out, 0, sizeof(out)) == 0);
	assert(memcmp(out, buf, sizeof(out)) == 0);

	memset(buf, 0x77, sizeof(buf));
	assert(scorpi_image_chain_write(chain, buf, 0, sizeof(buf)) == 0);
	assert(scorpi_image_chain_flush(chain) == 0);
	assert(scorpi_image_chain_close(chain) == 0);

	fd = open(path, O_RDONLY);
	assert(fd >= 0);
	state = NULL;
	assert(raw->open(path, fd, true, &state) == 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single_backend(raw, state, &chain) == 0);
	assert(scorpi_image_chain_write(chain, buf, 0, sizeof(buf)) == EROFS);
	assert(scorpi_image_chain_discard(chain, 0, sizeof(buf)) == EROFS);
	assert(scorpi_image_chain_close(chain) == 0);

	assert(unlink(path) == 0);
	test_raw_chain_diagnostics();
	test_concurrent_chain_read_write_serializes_cache();
	return (0);
}
