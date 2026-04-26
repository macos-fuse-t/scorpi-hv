/* .sco V1 parser and superblock selection coverage. */

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

#include "scorpi_image.h"
#include "scorpi_image_chain.h"

#define	SCO_MAGIC			"SCOIMG\0\0"
#define	SCO_FILE_ID_SIZE		0x1000
#define	SCO_SUPERBLOCK_SIZE		0x1000
#define	SCO_SUPERBLOCK_A_OFFSET		0x1000
#define	SCO_SUPERBLOCK_B_OFFSET		0x2000
#define	SCO_METADATA_AREA_OFFSET		0x10000
#define	SCO_MAP_ROOT_OFFSET		0x10000
#define	SCO_BASE_DESCRIPTOR_OFFSET	0x11000
#define	SCO_MAP_PAGE_OFFSET		0x12000
#define	SCO_DATA_AREA_OFFSET		0x40000
#define	SCO_CLUSTER_SIZE		0x40000
#define	SCO_CHECKSUM_CRC32C		1
#define	SCO_INCOMPAT_ALLOC_MAP_V1	1
#define	SCO_INCOMPAT_ZERO_DISCARD	2

#define	SCO_MAP_STATE_ABSENT		0
#define	SCO_MAP_STATE_PRESENT		1
#define	SCO_MAP_STATE_ZERO		2
#define	SCO_MAP_STATE_DISCARDED		3

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
	const char *base_uri;
	bool has_base_uuid;
	bool has_base_digest;
	bool corrupt_base_descriptor_crc;
	bool corrupt_base_descriptor_padding;
	bool write_map_root;
	bool map_page_present;
	bool corrupt_map_page_crc;
	uint8_t map_state;
	uint8_t data_byte;
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
	if (fixture->base_uri != NULL) {
		le64enc(buf + 0x0050, SCO_BASE_DESCRIPTOR_OFFSET);
		le32enc(buf + 0x0058, 0x1000);
	}
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
build_base_descriptor(uint8_t buf[0x1000],
    const struct superblock_fixture *fixture)
{
	uint32_t crc;
	size_t uri_len;

	memset(buf, 0, 0x1000);
	uri_len = strlen(fixture->base_uri);
	le32enc(buf + 0x0000, 0x1000);
	le32enc(buf + 0x0004, SCO_CHECKSUM_CRC32C);
	le32enc(buf + 0x0010, (uint32_t)uri_len);
	memcpy(buf + 0x0018, test_uuid, sizeof(test_uuid));
	memset(buf + 0x0028, 0x5a, 32);
	le32enc(buf + 0x0048, fixture->has_base_uuid ? 1 : 0);
	le32enc(buf + 0x004c, fixture->has_base_digest ? 1 : 0);
	memcpy(buf + 0x0050, fixture->base_uri, uri_len);
	if (fixture->corrupt_base_descriptor_padding)
		buf[0x0050 + uri_len] = 0xa5;
	crc = crc32c(buf, 0x1000);
	if (fixture->corrupt_base_descriptor_crc)
		crc ^= 0xffffffffU;
	le32enc(buf + 0x0008, crc);
}

static uint32_t
build_map_page(uint8_t buf[0x1000], const struct superblock_fixture *fixture)
{
	uint32_t crc;

	memset(buf, 0, 0x1000);
	le32enc(buf + 0x0000, 0x1000);
	le32enc(buf + 0x0004, SCO_CHECKSUM_CRC32C);
	le32enc(buf + 0x000c, 1);
	le64enc(buf + 0x0010, 0);
	buf[0x0018] = fixture->map_state;
	if (fixture->map_state == SCO_MAP_STATE_PRESENT)
		le64enc(buf + 0x0018 + 0x0008, SCO_DATA_AREA_OFFSET);
	crc = crc32c(buf, 0x1000);
	if (fixture->corrupt_map_page_crc)
		crc ^= 0xffffffffU;
	le32enc(buf + 0x0008, crc);
	return (crc);
}

static void
build_root_page(uint8_t buf[0x1000], uint32_t map_page_crc32c,
    const struct superblock_fixture *fixture)
{
	uint32_t crc;

	memset(buf, 0, 0x1000);
	le32enc(buf + 0x0000, 0x1000);
	le32enc(buf + 0x0004, SCO_CHECKSUM_CRC32C);
	le32enc(buf + 0x000c, 1);
	le64enc(buf + 0x0010, 0);
	if (fixture->map_page_present) {
		le64enc(buf + 0x0018, SCO_MAP_PAGE_OFFSET);
		le32enc(buf + 0x0018 + 0x0008, map_page_crc32c);
	}
	crc = crc32c(buf, 0x1000);
	le32enc(buf + 0x0008, crc);
}

static void
write_data_cluster(int fd, uint8_t data_byte)
{
	uint8_t buf[512];

	memset(buf, data_byte, sizeof(buf));
	assert(ftruncate(fd, SCO_DATA_AREA_OFFSET + SCO_CLUSTER_SIZE) == 0);
	write_exact(fd, SCO_DATA_AREA_OFFSET, buf, sizeof(buf));
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
	if (a->base_uri != NULL || b->base_uri != NULL) {
		const struct superblock_fixture *fixture;

		fixture = a->base_uri != NULL ? a : b;
		build_base_descriptor(superblock, fixture);
		write_exact(fd, SCO_BASE_DESCRIPTOR_OFFSET, superblock,
		    sizeof(superblock));
	}
	if (a->write_map_root || b->write_map_root) {
		const struct superblock_fixture *fixture;
		uint32_t map_page_crc32c;

		fixture = a->write_map_root ? a : b;
		map_page_crc32c = 0;
		if (fixture->map_page_present) {
			map_page_crc32c = build_map_page(superblock, fixture);
			write_exact(fd, SCO_MAP_PAGE_OFFSET, superblock,
			    sizeof(superblock));
		}
		build_root_page(superblock, map_page_crc32c, fixture);
		write_exact(fd, SCO_MAP_ROOT_OFFSET, superblock,
		    sizeof(superblock));
		if (fixture->map_state == SCO_MAP_STATE_PRESENT)
			write_data_cluster(fd, fixture->data_byte);
	}

	assert(close(fd) == 0);
}

static int
open_sco(const char *path, void **statep)
{
	const struct scorpi_image_ops *sco;
	int error, fd;

	sco = scorpi_image_backend_find("sco");
	assert(sco != NULL);
	fd = open(path, O_RDONLY);
	assert(fd >= 0);
	*statep = NULL;
	error = sco->open(path, fd, true, statep);
	if (error != 0)
		assert(close(fd) == 0);
	return (error);
}

static void
assert_buffer_value(const uint8_t *buf, size_t len, uint8_t expected)
{
	size_t i;

	for (i = 0; i < len; i++)
		assert(buf[i] == expected);
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
	sco = scorpi_image_backend_find("sco");
	assert(sco != NULL);
	assert(sco->get_info(state, &info) == 0);
	assert(info.format == SCORPI_IMAGE_FORMAT_SCO);
	assert(info.virtual_size == a.virtual_size);
	assert(info.logical_sector_size == 512);
	assert(info.cluster_size == SCO_CLUSTER_SIZE);
	assert(info.has_image_uuid);
	assert(!info.has_base);
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
	sco = scorpi_image_backend_find("sco");
	assert(sco != NULL);
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
	sco = scorpi_image_backend_find("sco");
	assert(sco != NULL);
	assert(sco->get_info(state, &info) == 0);
	assert(info.virtual_size == a.virtual_size);
	assert(sco->close(state) == 0);
	assert(unlink(path) == 0);
}

static void
test_base_descriptor_exposed(void)
{
	const struct scorpi_image_ops *sco;
	struct scorpi_image_info info;
	struct superblock_fixture a, b;
	char path[] = "/tmp/scorpi-sco-base-desc-XXXXXX";
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
		.base_uri = "file:base.raw",
		.has_base_uuid = true,
		.has_base_digest = true,
	};
	b = (struct superblock_fixture){ 0 };
	write_fixture(path, false, 1, false, &a, &b);

	state = NULL;
	assert(open_sco(path, &state) == 0);
	sco = scorpi_image_backend_find("sco");
	assert(sco != NULL);
	memset(&info, 0, sizeof(info));
	assert(sco->get_info(state, &info) == 0);
	assert(info.has_base);
	assert(strcmp(info.base_uri, "file:base.raw") == 0);
	assert(info.has_base_uuid);
	assert(memcmp(info.base_uuid, test_uuid, sizeof(test_uuid)) == 0);
	assert(info.has_base_digest);
	free(info.base_uri);
	assert(sco->close(state) == 0);
	assert(unlink(path) == 0);
}

static void
test_unsupported_base_uri_rejected(void)
{
	struct superblock_fixture a, b;
	char path[] = "/tmp/scorpi-sco-bad-uri-XXXXXX";
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
		.base_uri = "http://example.invalid/base.sco",
	};
	b = (struct superblock_fixture){ 0 };
	write_fixture(path, false, 1, false, &a, &b);
	assert(open_sco(path, &state) != 0);
	assert(unlink(path) == 0);
}

static void
test_base_descriptor_chain_resolves_raw(void)
{
	struct scorpi_image_chain_open_options options;
	struct scorpi_image_chain *chain;
	struct superblock_fixture a, b;
	const struct scorpi_image_info *info;
	char child[] = "/tmp/scorpi-sco-chain-child-XXXXXX";
	char base[] = "/tmp/scorpi-sco-chain-base-XXXXXX";
	char uri[128];
	int child_fd, base_fd;

	child_fd = mkstemp(child);
	assert(child_fd >= 0);
	assert(close(child_fd) == 0);
	base_fd = mkstemp(base);
	assert(base_fd >= 0);
	assert(ftruncate(base_fd, 128 * 1024 * 1024) == 0);
	assert(close(base_fd) == 0);

	a = (struct superblock_fixture){
		.present = true,
		.major = 1,
		.virtual_size = 128 * 1024 * 1024,
		.incompatible_features = SCO_INCOMPAT_ALLOC_MAP_V1,
		.base_uri = "file:scorpi-sco-chain-base-XXXXXX",
	};
	b = (struct superblock_fixture){ 0 };
	a.base_uri = strrchr(base, '/') + 1;
	snprintf(uri, sizeof(uri), "file:%s", a.base_uri);
	a.base_uri = uri;
	write_fixture(child, false, 1, false, &a, &b);

	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = true,
	};
	child_fd = open(child, O_RDONLY);
	assert(child_fd >= 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single(child, child_fd, true, &options,
	    &chain) == 0);
	assert(scorpi_image_chain_layer_count(chain) == 2);
	info = scorpi_image_chain_layer_info(chain, 0);
	assert(info != NULL && info->format == SCORPI_IMAGE_FORMAT_SCO);
	info = scorpi_image_chain_layer_info(chain, 1);
	assert(info != NULL && info->format == SCORPI_IMAGE_FORMAT_RAW);
	assert(scorpi_image_chain_close(chain) == 0);
	assert(unlink(child) == 0);
	assert(unlink(base) == 0);
}

static void
test_map_present_reads_from_sco(void)
{
	const struct scorpi_image_ops *sco;
	struct scorpi_image_extent extent;
	struct superblock_fixture a, b;
	uint8_t buf[512];
	char path[] = "/tmp/scorpi-sco-map-present-XXXXXX";
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
		.write_map_root = true,
		.map_page_present = true,
		.map_state = SCO_MAP_STATE_PRESENT,
		.data_byte = 0x34,
	};
	b = (struct superblock_fixture){ 0 };
	write_fixture(path, false, 1, false, &a, &b);

	assert(open_sco(path, &state) == 0);
	sco = scorpi_image_backend_find("sco");
	assert(sco != NULL);
	assert(sco->map(state, 0, sizeof(buf), &extent) == 0);
	assert(extent.state == SCORPI_IMAGE_EXTENT_PRESENT);
	assert(extent.offset == 0);
	assert(extent.length == sizeof(buf));
	assert(sco->read(state, buf, 0, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 0x34);
	assert(sco->close(state) == 0);
	assert(unlink(path) == 0);
}

static void
test_map_absent_reports_absent(void)
{
	const struct scorpi_image_ops *sco;
	struct scorpi_image_extent extent;
	struct superblock_fixture a, b;
	char path[] = "/tmp/scorpi-sco-map-absent-XXXXXX";
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
		.write_map_root = true,
		.map_page_present = false,
	};
	b = (struct superblock_fixture){ 0 };
	write_fixture(path, false, 1, false, &a, &b);

	assert(open_sco(path, &state) == 0);
	sco = scorpi_image_backend_find("sco");
	assert(sco != NULL);
	assert(sco->map(state, 0, 4096, &extent) == 0);
	assert(extent.state == SCORPI_IMAGE_EXTENT_ABSENT);
	assert(extent.offset == 0);
	assert(extent.length == 4096);
	assert(sco->close(state) == 0);
	assert(unlink(path) == 0);
}

static void
test_map_zero_and_discarded_read_zero(void)
{
	const struct scorpi_image_ops *sco;
	struct superblock_fixture a, b;
	struct scorpi_image_extent extent;
	uint8_t buf[512];
	char zero_path[] = "/tmp/scorpi-sco-map-zero-XXXXXX";
	char discarded_path[] = "/tmp/scorpi-sco-map-discarded-XXXXXX";
	void *state;
	int fd;

	fd = mkstemp(zero_path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	a = (struct superblock_fixture){
		.present = true,
		.major = 1,
		.virtual_size = 128 * 1024 * 1024,
		.incompatible_features = SCO_INCOMPAT_ALLOC_MAP_V1 |
		    SCO_INCOMPAT_ZERO_DISCARD,
		.write_map_root = true,
		.map_page_present = true,
		.map_state = SCO_MAP_STATE_ZERO,
	};
	b = (struct superblock_fixture){ 0 };
	write_fixture(zero_path, false, 1, false, &a, &b);

	assert(open_sco(zero_path, &state) == 0);
	sco = scorpi_image_backend_find("sco");
	assert(sco != NULL);
	assert(sco->map(state, 0, sizeof(buf), &extent) == 0);
	assert(extent.state == SCORPI_IMAGE_EXTENT_ZERO);
	memset(buf, 0xff, sizeof(buf));
	assert(sco->read(state, buf, 0, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 0);
	assert(sco->close(state) == 0);
	assert(unlink(zero_path) == 0);

	fd = mkstemp(discarded_path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	a.map_state = SCO_MAP_STATE_DISCARDED;
	write_fixture(discarded_path, false, 1, false, &a, &b);
	assert(open_sco(discarded_path, &state) == 0);
	assert(sco->map(state, 0, sizeof(buf), &extent) == 0);
	assert(extent.state == SCORPI_IMAGE_EXTENT_DISCARDED);
	memset(buf, 0xff, sizeof(buf));
	assert(sco->read(state, buf, 0, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 0);
	assert(sco->close(state) == 0);
	assert(unlink(discarded_path) == 0);
}

static void
test_corrupt_map_page_rejected(void)
{
	const struct scorpi_image_ops *sco;
	struct scorpi_image_extent extent;
	struct superblock_fixture a, b;
	char path[] = "/tmp/scorpi-sco-map-corrupt-XXXXXX";
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
		.write_map_root = true,
		.map_page_present = true,
		.map_state = SCO_MAP_STATE_ABSENT,
		.corrupt_map_page_crc = true,
	};
	b = (struct superblock_fixture){ 0 };
	write_fixture(path, false, 1, false, &a, &b);

	assert(open_sco(path, &state) == 0);
	sco = scorpi_image_backend_find("sco");
	assert(sco != NULL);
	assert(sco->map(state, 0, 4096, &extent) != 0);
	assert(sco->close(state) == 0);
	assert(unlink(path) == 0);
}

static void
test_absent_map_falls_through_to_base(void)
{
	struct scorpi_image_chain_open_options options;
	struct scorpi_image_chain *chain;
	struct superblock_fixture a, b;
	uint8_t buf[512], base_buf[512];
	char child[] = "/tmp/scorpi-sco-absent-child-XXXXXX";
	char base[] = "/tmp/scorpi-sco-absent-base-XXXXXX";
	char uri[128];
	int child_fd, base_fd;

	child_fd = mkstemp(child);
	assert(child_fd >= 0);
	assert(close(child_fd) == 0);
	base_fd = mkstemp(base);
	assert(base_fd >= 0);
	assert(ftruncate(base_fd, 128 * 1024 * 1024) == 0);
	memset(base_buf, 0x45, sizeof(base_buf));
	write_exact(base_fd, 0, base_buf, sizeof(base_buf));
	assert(close(base_fd) == 0);

	snprintf(uri, sizeof(uri), "file:%s", strrchr(base, '/') + 1);
	a = (struct superblock_fixture){
		.present = true,
		.major = 1,
		.virtual_size = 128 * 1024 * 1024,
		.incompatible_features = SCO_INCOMPAT_ALLOC_MAP_V1,
		.base_uri = uri,
		.write_map_root = true,
		.map_page_present = false,
	};
	b = (struct superblock_fixture){ 0 };
	write_fixture(child, false, 1, false, &a, &b);

	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = true,
	};
	child_fd = open(child, O_RDONLY);
	assert(child_fd >= 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single(child, child_fd, true, &options,
	    &chain) == 0);
	memset(buf, 0, sizeof(buf));
	assert(scorpi_image_chain_read(chain, buf, 0, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 0x45);
	assert(scorpi_image_chain_close(chain) == 0);
	assert(unlink(child) == 0);
	assert(unlink(base) == 0);
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
	test_base_descriptor_exposed();
	test_unsupported_base_uri_rejected();
	test_base_descriptor_chain_resolves_raw();
	test_map_present_reads_from_sco();
	test_map_absent_reports_absent();
	test_map_zero_and_discarded_read_zero();
	test_corrupt_map_page_rejected();
	test_absent_map_falls_through_to_base();
	return (0);
}
