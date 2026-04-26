/* Raw image backend coverage for snapshot support. */

#include <sys/errno.h>
#include <sys/stat.h>

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

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
	const struct scorpi_image_ops *raw;
	struct scorpi_image_extent extent;
	struct scorpi_image_info info;
	char path[] = "/tmp/scorpi-raw-test-XXXXXX";
	uint8_t buf[16], out[16];
	uint32_t score;
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	memset(buf, 0x41, sizeof(buf));
	write_all(fd, buf, sizeof(buf));
	close(fd);

	raw = scorpi_image_raw_backend();
	score = 0;
	assert(raw->probe(-1, &score) == 0);
	assert(score == 1);

	fd = open(path, O_RDWR);
	assert(fd >= 0);
	state = NULL;
	assert(raw->open(path, fd, false, &state) == 0);
	assert(raw->get_info(state, &info) == 0);
	assert(info.format == SCORPI_IMAGE_FORMAT_RAW);
	assert(info.virtual_size == sizeof(buf));
	assert(info.logical_sector_size == 512);
	assert(!info.readonly);
	assert(!info.has_parent);

	assert(raw->map(state, 0, sizeof(buf), &extent) == 0);
	assert(extent.offset == 0);
	assert(extent.length == sizeof(buf));
	assert(extent.state == SCORPI_IMAGE_EXTENT_PRESENT);

	memset(out, 0, sizeof(out));
	assert(raw->read(state, out, 0, sizeof(out)) == 0);
	assert(memcmp(out, buf, sizeof(out)) == 0);

	memset(buf, 0x52, sizeof(buf));
	assert(raw->write(state, buf, 0, sizeof(buf)) == 0);
	assert(raw->flush(state) == 0);
	assert(raw->close(state) == 0);

	fd = open(path, O_RDONLY);
	assert(fd >= 0);
	state = NULL;
	assert(raw->open(path, fd, true, &state) == 0);
	assert(raw->get_info(state, &info) == 0);
	assert(info.readonly);
	assert(info.sealed);
	assert(raw->write(state, buf, 0, sizeof(buf)) == EROFS);
	assert(raw->discard(state, 0, sizeof(buf)) == EOPNOTSUPP);
	assert(raw->close(state) == 0);

	assert(unlink(path) == 0);
	return (0);
}
