/* .sco V1 parser and superblock selection coverage. */

#include <sys/errno.h>

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <support/endian.h>

#include "scorpi_image_sco.h"

#define	SCO_MAGIC			"SCOIMG\0\0"
#define	SCO_FILE_ID_SIZE		0x1000
#define	SCO_SUPERBLOCK_SIZE		0x1000
#define	SCO_SUPERBLOCK_A_OFFSET		0x1000
#define	SCO_SUPERBLOCK_B_OFFSET		0x2000
#define	SCO_METADATA_AREA_OFFSET		0x10000
#define	SCO_MAP_ROOT_OFFSET		0x10000
#define	SCO_DATA_AREA_OFFSET		0x40000
#define	SCO_CLUSTER_SIZE		0x40000
#define	SCO_CHECKSUM_CRC32C		1
#define	SCO_INCOMPAT_ALLOC_MAP_V1	1

static const uint8_t test_uuid[16] = {
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

struct superblock_fixture {
	bool present;
	bool corrupt_crc;
	uint16_t major;
	uint16_t minor;
	uint64_t generation;
	uint64_t virtual_size;
	uint32_t incompatible_features;
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
cluster_count(uint64_t virtual_size)
{
	return ((virtual_size + SCO_CLUSTER_SIZE - 1) / SCO_CLUSTER_SIZE);
}

static void
write_exact(int fd, uint64_t offset, const void *buf, size_t len)
{
	assert(pwrite(fd, buf, len, (off_t)offset) == (ssize_t)len);
}

static void
build_file_id(uint8_t buf[SCO_FILE_ID_SIZE], bool bad_magic,
    uint16_t major, bool corrupt_crc)
{
	uint32_t crc;

	memset(buf, 0, SCO_FILE_ID_SIZE);
	memcpy(buf, bad_magic ? "BADIMG\0\0" : SCO_MAGIC, 8);
	le16enc(buf + 0x0008, major);
	le16enc(buf + 0x000a, 0);
	le32enc(buf + 0x000c, SCO_FILE_ID_SIZE);
	memcpy(buf + 0x0010, test_uuid, sizeof(test_uuid));
	le32enc(buf + 0x0020, SCO_CHECKSUM_CRC32C);
	crc = crc32c(buf, SCO_FILE_ID_SIZE);
	if (corrupt_crc)
		crc ^= 0xffffffffU;
	le32enc(buf + 0x0024, crc);
}

static void
build_superblock(uint8_t buf[SCO_SUPERBLOCK_SIZE],
    const struct superblock_fixture *fixture)
{
	uint32_t crc;

	memset(buf, 0, SCO_SUPERBLOCK_SIZE);
	if (!fixture->present)
		return;

	memcpy(buf, SCO_MAGIC, 8);
	le16enc(buf + 0x0008, fixture->major);
	le16enc(buf + 0x000a, fixture->minor);
	le32enc(buf + 0x000c, SCO_SUPERBLOCK_SIZE);
	le32enc(buf + 0x0010, SCO_CHECKSUM_CRC32C);
	le64enc(buf + 0x0018, fixture->generation);
	le64enc(buf + 0x0020, fixture->virtual_size);
	le32enc(buf + 0x0028, 512);
	le32enc(buf + 0x002c, 4096);
	le32enc(buf + 0x0030, SCO_CLUSTER_SIZE);
	le64enc(buf + 0x0038, cluster_count(fixture->virtual_size));
	le64enc(buf + 0x0040, SCO_METADATA_AREA_OFFSET);
	le64enc(buf + 0x0048, SCO_DATA_AREA_OFFSET);
	le64enc(buf + 0x0060, SCO_MAP_ROOT_OFFSET);
	le32enc(buf + 0x0068, 0x1000);
	le32enc(buf + 0x006c, 16);
	le32enc(buf + 0x0070, fixture->incompatible_features);
	memcpy(buf + 0x0080, test_uuid, sizeof(test_uuid));
	crc = crc32c(buf, SCO_SUPERBLOCK_SIZE);
	if (fixture->corrupt_crc)
		crc ^= 0xffffffffU;
	le32enc(buf + 0x0014, crc);
}

static void
write_fixture(const char *path, bool bad_magic, uint16_t file_major,
    bool corrupt_file_id_crc, const struct superblock_fixture *a,
    const struct superblock_fixture *b)
{
	uint8_t file_id[SCO_FILE_ID_SIZE], superblock[SCO_SUPERBLOCK_SIZE];
	int fd;

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	assert(fd >= 0);
	assert(ftruncate(fd, SCO_DATA_AREA_OFFSET) == 0);

	build_file_id(file_id, bad_magic, file_major, corrupt_file_id_crc);
	write_exact(fd, 0, file_id, sizeof(file_id));
	build_superblock(superblock, a);
	write_exact(fd, SCO_SUPERBLOCK_A_OFFSET, superblock, sizeof(superblock));
	build_superblock(superblock, b);
	write_exact(fd, SCO_SUPERBLOCK_B_OFFSET, superblock, sizeof(superblock));

	assert(close(fd) == 0);
}

static int
open_sco(const char *path, void **statep)
{
	const struct scorpi_image_ops *sco;
	int error, fd;

	sco = scorpi_image_sco_backend();
	fd = open(path, O_RDONLY);
	assert(fd >= 0);
	*statep = NULL;
	error = sco->open(path, fd, true, statep);
	if (error != 0)
		assert(close(fd) == 0);
	return (error);
}

static void
test_valid_sco_opens(void)
{
	const struct scorpi_image_ops *sco;
	struct scorpi_image_info info;
	struct superblock_fixture a, b;
	char path[] = "/tmp/scorpi-sco-valid-XXXXXX";
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	a = (struct superblock_fixture){
		.present = true,
		.major = 1,
		.minor = 0,
		.generation = 1,
		.virtual_size = 128 * 1024 * 1024,
		.incompatible_features = SCO_INCOMPAT_ALLOC_MAP_V1,
	};
	b = (struct superblock_fixture){ 0 };
	write_fixture(path, false, 1, false, &a, &b);

	state = NULL;
	assert(open_sco(path, &state) == 0);
	sco = scorpi_image_sco_backend();
	assert(sco->get_info(state, &info) == 0);
	assert(info.format == SCORPI_IMAGE_FORMAT_SCO);
	assert(info.virtual_size == a.virtual_size);
	assert(info.logical_sector_size == 512);
	assert(info.cluster_size == SCO_CLUSTER_SIZE);
	assert(sco->close(state) == 0);
	assert(unlink(path) == 0);
}

static void
test_bad_magic_rejected(void)
{
	struct superblock_fixture a, b;
	char path[] = "/tmp/scorpi-sco-magic-XXXXXX";
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	a = (struct superblock_fixture){
		.present = true,
		.major = 1,
		.virtual_size = 128 * 1024 * 1024,
		.incompatible_features = SCO_INCOMPAT_ALLOC_MAP_V1,
	};
	b = (struct superblock_fixture){ 0 };
	write_fixture(path, true, 1, false, &a, &b);
	assert(open_sco(path, &state) != 0);
	assert(unlink(path) == 0);
}

static void
test_unsupported_version_rejected(void)
{
	struct superblock_fixture a, b;
	char path[] = "/tmp/scorpi-sco-version-XXXXXX";
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	a = (struct superblock_fixture){
		.present = true,
		.major = 1,
		.virtual_size = 128 * 1024 * 1024,
		.incompatible_features = SCO_INCOMPAT_ALLOC_MAP_V1,
	};
	b = (struct superblock_fixture){ 0 };
	write_fixture(path, false, 2, false, &a, &b);
	assert(open_sco(path, &state) != 0);
	assert(unlink(path) == 0);
}

static void
test_unknown_incompatible_feature_rejected(void)
{
	struct superblock_fixture a, b;
	char path[] = "/tmp/scorpi-sco-feature-XXXXXX";
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	a = (struct superblock_fixture){
		.present = true,
		.major = 1,
		.virtual_size = 128 * 1024 * 1024,
		.incompatible_features = SCO_INCOMPAT_ALLOC_MAP_V1 |
		    (1U << 31),
	};
	b = (struct superblock_fixture){ 0 };
	write_fixture(path, false, 1, false, &a, &b);
	assert(open_sco(path, &state) != 0);
	assert(unlink(path) == 0);
}

static void
test_invalid_checksum_rejected(void)
{
	struct superblock_fixture a, b;
	char path[] = "/tmp/scorpi-sco-checksum-XXXXXX";
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	a = (struct superblock_fixture){
		.present = true,
		.corrupt_crc = true,
		.major = 1,
		.virtual_size = 128 * 1024 * 1024,
		.incompatible_features = SCO_INCOMPAT_ALLOC_MAP_V1,
	};
	b = (struct superblock_fixture){ 0 };
	write_fixture(path, false, 1, false, &a, &b);
	assert(open_sco(path, &state) != 0);
	assert(unlink(path) == 0);
}

static void
test_newest_valid_generation_selected(void)
{
	const struct scorpi_image_ops *sco;
	struct scorpi_image_info info;
	struct superblock_fixture a, b;
	char path[] = "/tmp/scorpi-sco-generation-XXXXXX";
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	a = (struct superblock_fixture){
		.present = true,
		.major = 1,
		.generation = 1,
		.virtual_size = 128 * 1024 * 1024,
		.incompatible_features = SCO_INCOMPAT_ALLOC_MAP_V1,
	};
	b = (struct superblock_fixture){
		.present = true,
		.major = 1,
		.generation = 2,
		.virtual_size = 256 * 1024 * 1024,
		.incompatible_features = SCO_INCOMPAT_ALLOC_MAP_V1,
	};
	write_fixture(path, false, 1, false, &a, &b);
	assert(open_sco(path, &state) == 0);
	sco = scorpi_image_sco_backend();
	assert(sco->get_info(state, &info) == 0);
	assert(info.virtual_size == b.virtual_size);
	assert(sco->close(state) == 0);
	assert(unlink(path) == 0);
}

static void
test_corrupt_newest_generation_falls_back(void)
{
	const struct scorpi_image_ops *sco;
	struct scorpi_image_info info;
	struct superblock_fixture a, b;
	char path[] = "/tmp/scorpi-sco-fallback-XXXXXX";
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	a = (struct superblock_fixture){
		.present = true,
		.major = 1,
		.generation = 1,
		.virtual_size = 128 * 1024 * 1024,
		.incompatible_features = SCO_INCOMPAT_ALLOC_MAP_V1,
	};
	b = (struct superblock_fixture){
		.present = true,
		.corrupt_crc = true,
		.major = 1,
		.generation = 2,
		.virtual_size = 256 * 1024 * 1024,
		.incompatible_features = SCO_INCOMPAT_ALLOC_MAP_V1,
	};
	write_fixture(path, false, 1, false, &a, &b);
	assert(open_sco(path, &state) == 0);
	sco = scorpi_image_sco_backend();
	assert(sco->get_info(state, &info) == 0);
	assert(info.virtual_size == a.virtual_size);
	assert(sco->close(state) == 0);
	assert(unlink(path) == 0);
}

int
main(void)
{
	test_valid_sco_opens();
	test_bad_magic_rejected();
	test_unsupported_version_rejected();
	test_unknown_incompatible_feature_rejected();
	test_invalid_checksum_rejected();
	test_newest_valid_generation_selected();
	test_corrupt_newest_generation_falls_back();
	return (0);
}
