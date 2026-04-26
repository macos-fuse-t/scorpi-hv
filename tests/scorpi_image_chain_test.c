/* One-layer image chain coverage for snapshot support. */

#include <sys/errno.h>

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "scorpi_image_chain.h"
#include "scorpi_image_raw.h"

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
	assert(scorpi_image_chain_open_single(raw, state, &chain) == 0);

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
	assert(scorpi_image_chain_open_single(raw, state, &chain) == 0);
	assert(scorpi_image_chain_write(chain, buf, 0, sizeof(buf)) == EROFS);
	assert(scorpi_image_chain_discard(chain, 0, sizeof(buf)) == EROFS);
	assert(scorpi_image_chain_close(chain) == 0);

	assert(unlink(path) == 0);
	return (0);
}
