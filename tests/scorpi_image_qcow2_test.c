/* Readonly qcow2 backend coverage for snapshot support. */

#include <sys/errno.h>

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <support/endian.h>

#include "scorpi_image_chain.h"
#include "scorpi_image_qcow2.h"

#define	QCOW2_TEST_CLUSTER_BITS	16U
#define	QCOW2_TEST_CLUSTER_SIZE	(1U << QCOW2_TEST_CLUSTER_BITS)
#define	QCOW2_TEST_VIRTUAL_SIZE	(3ULL * QCOW2_TEST_CLUSTER_SIZE)
#define	QCOW2_TEST_L1_OFFSET	QCOW2_TEST_CLUSTER_SIZE
#define	QCOW2_TEST_L2_OFFSET	(2ULL * QCOW2_TEST_CLUSTER_SIZE)
#define	QCOW2_TEST_DATA_OFFSET	(3ULL * QCOW2_TEST_CLUSTER_SIZE)
#define	QCOW2_TEST_COPIED	(1ULL << 63)
#define	QCOW2_TEST_ZERO		(1ULL << 0)

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

static void
pwrite_all(int fd, uint64_t offset, const void *buf, size_t len)
{
	const uint8_t *p;
	ssize_t n;

	p = buf;
	while (len > 0) {
		n = pwrite(fd, p, len, (off_t)offset);
		assert(n > 0);
		p += n;
		offset += (uint64_t)n;
		len -= (size_t)n;
	}
}

static void
create_raw_base(const char *path)
{
	uint8_t buf[4096];
	uint64_t remaining;
	int fd;

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	assert(fd >= 0);
	memset(buf, 0x7b, sizeof(buf));
	remaining = QCOW2_TEST_VIRTUAL_SIZE;
	while (remaining > 0) {
		write_all(fd, buf, sizeof(buf));
		remaining -= sizeof(buf);
	}
	assert(close(fd) == 0);
}

static void
create_qcow2(const char *path, const char *base_name,
    uint64_t incompatible_features)
{
	uint8_t *cluster;
	size_t base_len;
	int fd;

	cluster = calloc(1, QCOW2_TEST_CLUSTER_SIZE);
	assert(cluster != NULL);

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	assert(fd >= 0);

	be32enc(cluster + 0, 0x514649fbU);
	be32enc(cluster + 4, 3);
	if (base_name != NULL) {
		base_len = strlen(base_name);
		assert(base_len <= 1023);
		be64enc(cluster + 8, 112);
		be32enc(cluster + 16, (uint32_t)base_len);
		memcpy(cluster + 112, base_name, base_len);
	}
	be32enc(cluster + 20, QCOW2_TEST_CLUSTER_BITS);
	be64enc(cluster + 24, QCOW2_TEST_VIRTUAL_SIZE);
	be32enc(cluster + 32, 0);
	be32enc(cluster + 36, 1);
	be64enc(cluster + 40, QCOW2_TEST_L1_OFFSET);
	be64enc(cluster + 48, 0);
	be32enc(cluster + 56, 0);
	be32enc(cluster + 60, 0);
	be64enc(cluster + 64, 0);
	be64enc(cluster + 72, incompatible_features);
	be64enc(cluster + 80, 0);
	be64enc(cluster + 88, 0);
	be32enc(cluster + 96, 4);
	be32enc(cluster + 100, 104);
	pwrite_all(fd, 0, cluster, QCOW2_TEST_CLUSTER_SIZE);

	memset(cluster, 0, QCOW2_TEST_CLUSTER_SIZE);
	be64enc(cluster, QCOW2_TEST_L2_OFFSET | QCOW2_TEST_COPIED);
	pwrite_all(fd, QCOW2_TEST_L1_OFFSET, cluster, QCOW2_TEST_CLUSTER_SIZE);

	memset(cluster, 0, QCOW2_TEST_CLUSTER_SIZE);
	be64enc(cluster, QCOW2_TEST_DATA_OFFSET | QCOW2_TEST_COPIED);
	be64enc(cluster + 8, QCOW2_TEST_ZERO);
	pwrite_all(fd, QCOW2_TEST_L2_OFFSET, cluster, QCOW2_TEST_CLUSTER_SIZE);

	memset(cluster, 0x5a, QCOW2_TEST_CLUSTER_SIZE);
	pwrite_all(fd, QCOW2_TEST_DATA_OFFSET, cluster, QCOW2_TEST_CLUSTER_SIZE);
	assert(close(fd) == 0);
	free(cluster);
}

static void
test_direct_qcow2_backend(void)
{
	const struct scorpi_image_ops *qcow2;
	struct scorpi_image_extent extent;
	struct scorpi_image_info info;
	char path[] = "/tmp/scorpi-qcow2-direct-XXXXXX";
	uint8_t buf[32];
	uint32_t score;
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	create_qcow2(path, NULL, 0);

	qcow2 = scorpi_image_qcow2_backend();
	fd = open(path, O_RDONLY);
	assert(fd >= 0);
	score = 0;
	assert(qcow2->probe(fd, &score) == 0);
	assert(score == 90);
	state = NULL;
	assert(qcow2->open(path, fd, true, &state) == 0);
	assert(qcow2->get_info(state, &info) == 0);
	assert(info.format == SCORPI_IMAGE_FORMAT_QCOW2);
	assert(info.virtual_size == QCOW2_TEST_VIRTUAL_SIZE);
	assert(info.logical_sector_size == 512);
	assert(info.cluster_size == QCOW2_TEST_CLUSTER_SIZE);
	assert(info.readonly);
	assert(info.sealed);
	assert(!info.has_base);

	memset(buf, 0, sizeof(buf));
	assert(qcow2->map(state, 128, sizeof(buf), &extent) == 0);
	assert(extent.offset == 128);
	assert(extent.length == sizeof(buf));
	assert(extent.state == SCORPI_IMAGE_EXTENT_PRESENT);
	assert(qcow2->read(state, buf, 128, sizeof(buf)) == 0);
	assert(buf[0] == 0x5a && buf[sizeof(buf) - 1] == 0x5a);

	memset(buf, 0xff, sizeof(buf));
	assert(qcow2->map(state, QCOW2_TEST_CLUSTER_SIZE + 128,
	    sizeof(buf), &extent) == 0);
	assert(extent.state == SCORPI_IMAGE_EXTENT_ZERO);
	assert(qcow2->read(state, buf, QCOW2_TEST_CLUSTER_SIZE + 128,
	    sizeof(buf)) == 0);
	assert(memcmp(buf, (uint8_t[32]){ 0 }, sizeof(buf)) == 0);

	assert(qcow2->write(state, buf, 0, sizeof(buf)) == EROFS);
	assert(qcow2->discard(state, 0, sizeof(buf)) == EROFS);
	assert(qcow2->close(state) == 0);

	fd = open(path, O_RDONLY);
	assert(fd >= 0);
	state = NULL;
	assert(qcow2->open(path, fd, false, &state) == EROFS);
	assert(close(fd) == 0);
	assert(unlink(path) == 0);
}

static void
test_qcow2_backing_chain(void)
{
	struct scorpi_image_chain_open_options options;
	struct scorpi_image_chain *chain;
	char dir[] = "/tmp/scorpi-qcow2-chain-XXXXXX";
	char qcow_path[256], raw_path[256];
	uint8_t buf[16];
	int fd;

	assert(mkdtemp(dir) != NULL);
	snprintf(qcow_path, sizeof(qcow_path), "%s/child.qcow2", dir);
	snprintf(raw_path, sizeof(raw_path), "%s/base.raw", dir);
	create_raw_base(raw_path);
	create_qcow2(qcow_path, "base.raw", 0);

	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = true,
	};
	fd = open(qcow_path, O_RDONLY);
	assert(fd >= 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single(qcow_path, fd, true, &options,
	    &chain) == 0);
	assert(scorpi_image_chain_layer_count(chain) == 2);

	memset(buf, 0, sizeof(buf));
	assert(scorpi_image_chain_read(chain, buf,
	    2ULL * QCOW2_TEST_CLUSTER_SIZE + 4096, sizeof(buf)) == 0);
	assert(buf[0] == 0x7b && buf[sizeof(buf) - 1] == 0x7b);
	assert(scorpi_image_chain_close(chain) == 0);

	assert(unlink(qcow_path) == 0);
	assert(unlink(raw_path) == 0);
	assert(rmdir(dir) == 0);
}

static void
test_qcow2_rejects_unsupported_features(void)
{
	const struct scorpi_image_ops *qcow2;
	char path[] = "/tmp/scorpi-qcow2-bad-XXXXXX";
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	create_qcow2(path, NULL, 1ULL << 4);

	qcow2 = scorpi_image_qcow2_backend();
	fd = open(path, O_RDONLY);
	assert(fd >= 0);
	state = NULL;
	assert(qcow2->open(path, fd, true, &state) == ENOTSUP);
	assert(close(fd) == 0);
	assert(unlink(path) == 0);
}

int
main(void)
{
	test_direct_qcow2_backend();
	test_qcow2_backing_chain();
	test_qcow2_rejects_unsupported_features();
	return (0);
}
