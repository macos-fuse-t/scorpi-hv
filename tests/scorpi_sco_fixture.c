/* .sco test fixture generator. */

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <support/endian.h>

#include "scorpi_sco_fixture.h"

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
#define	SCO_CHECKSUM_CRC32C		1

const uint8_t scorpi_sco_fixture_uuid[16] = {
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

static uint32_t
fixture_crc32c(const void *buf, size_t len)
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
fixture_cluster_count(uint64_t virtual_size)
{
	return ((virtual_size + SCORPI_SCO_FIXTURE_CLUSTER_SIZE - 1) /
	    SCORPI_SCO_FIXTURE_CLUSTER_SIZE);
}

static void
fixture_write_exact(int fd, uint64_t offset, const void *buf, size_t len)
{
	assert(pwrite(fd, buf, len, (off_t)offset) == (ssize_t)len);
}

static void
fixture_build_file_id(uint8_t buf[SCO_FILE_ID_SIZE], bool bad_magic,
    uint16_t major, bool corrupt_crc)
{
	uint32_t crc;

	memset(buf, 0, SCO_FILE_ID_SIZE);
	memcpy(buf, bad_magic ? "BADIMG\0\0" : SCO_MAGIC, 8);
	le16enc(buf + 0x0008, major);
	le32enc(buf + 0x000c, SCO_FILE_ID_SIZE);
	memcpy(buf + 0x0010, scorpi_sco_fixture_uuid,
	    sizeof(scorpi_sco_fixture_uuid));
	le32enc(buf + 0x0020, SCO_CHECKSUM_CRC32C);
	crc = fixture_crc32c(buf, SCO_FILE_ID_SIZE);
	if (corrupt_crc)
		crc ^= 0xffffffffU;
	le32enc(buf + 0x0024, crc);
}

static void
fixture_build_superblock(uint8_t buf[SCO_SUPERBLOCK_SIZE],
    const struct scorpi_sco_fixture_layer *fixture)
{
	uint64_t virtual_size;
	uint32_t crc;

	memset(buf, 0, SCO_SUPERBLOCK_SIZE);
	if (!fixture->present)
		return;

	virtual_size = fixture->virtual_size != 0 ? fixture->virtual_size :
	    SCORPI_SCO_FIXTURE_VIRTUAL_SIZE;
	memcpy(buf, SCO_MAGIC, 8);
	le16enc(buf + 0x0008, fixture->major);
	le16enc(buf + 0x000a, fixture->minor);
	le32enc(buf + 0x000c, SCO_SUPERBLOCK_SIZE);
	le32enc(buf + 0x0010, SCO_CHECKSUM_CRC32C);
	le64enc(buf + 0x0018, fixture->generation);
	le64enc(buf + 0x0020, virtual_size);
	le32enc(buf + 0x0028, 512);
	le32enc(buf + 0x002c, 4096);
	le32enc(buf + 0x0030, SCORPI_SCO_FIXTURE_CLUSTER_SIZE);
	le64enc(buf + 0x0038, fixture_cluster_count(virtual_size));
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
	memcpy(buf + 0x0080, scorpi_sco_fixture_uuid,
	    sizeof(scorpi_sco_fixture_uuid));
	crc = fixture_crc32c(buf, SCO_SUPERBLOCK_SIZE);
	if (fixture->corrupt_superblock_crc)
		crc ^= 0xffffffffU;
	le32enc(buf + 0x0014, crc);
}

static void
fixture_build_base_descriptor(uint8_t buf[0x1000],
    const struct scorpi_sco_fixture_layer *fixture)
{
	uint32_t crc;
	size_t uri_len;

	memset(buf, 0, 0x1000);
	uri_len = strlen(fixture->base_uri);
	le32enc(buf + 0x0000, 0x1000);
	le32enc(buf + 0x0004, SCO_CHECKSUM_CRC32C);
	le32enc(buf + 0x0010, (uint32_t)uri_len);
	memcpy(buf + 0x0018, scorpi_sco_fixture_uuid,
	    sizeof(scorpi_sco_fixture_uuid));
	memset(buf + 0x0028, 0x5a, 32);
	le32enc(buf + 0x0048, fixture->has_base_uuid ? 1 : 0);
	le32enc(buf + 0x004c, fixture->has_base_digest ? 1 : 0);
	memcpy(buf + 0x0050, fixture->base_uri, uri_len);
	if (fixture->corrupt_base_descriptor_padding)
		buf[0x0050 + uri_len] = 0xa5;
	crc = fixture_crc32c(buf, 0x1000);
	if (fixture->corrupt_base_descriptor_crc)
		crc ^= 0xffffffffU;
	le32enc(buf + 0x0008, crc);
}

static uint32_t
fixture_build_map_page(uint8_t buf[0x1000],
    const struct scorpi_sco_fixture_layer *fixture)
{
	uint32_t crc;

	memset(buf, 0, 0x1000);
	le32enc(buf + 0x0000, 0x1000);
	le32enc(buf + 0x0004, SCO_CHECKSUM_CRC32C);
	le32enc(buf + 0x000c, 1);
	le64enc(buf + 0x0010, 0);
	buf[0x0018] = fixture->map_state;
	if (fixture->map_state == SCORPI_SCO_FIXTURE_MAP_PRESENT)
		le64enc(buf + 0x0018 + 0x0008, SCO_DATA_AREA_OFFSET);
	crc = fixture_crc32c(buf, 0x1000);
	if (fixture->corrupt_map_page_crc)
		crc ^= 0xffffffffU;
	le32enc(buf + 0x0008, crc);
	return (crc);
}

static void
fixture_build_root_page(uint8_t buf[0x1000], uint32_t map_page_crc32c,
    const struct scorpi_sco_fixture_layer *fixture)
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
	crc = fixture_crc32c(buf, 0x1000);
	le32enc(buf + 0x0008, crc);
}

static void
fixture_write_data_cluster(int fd, uint8_t data_byte)
{
	uint8_t buf[512];

	memset(buf, data_byte, sizeof(buf));
	assert(ftruncate(fd, SCO_DATA_AREA_OFFSET +
	    SCORPI_SCO_FIXTURE_CLUSTER_SIZE) == 0);
	fixture_write_exact(fd, SCO_DATA_AREA_OFFSET, buf, sizeof(buf));
}

void
scorpi_sco_fixture_write(const char *path, bool bad_magic,
    uint16_t file_major, bool corrupt_file_id_crc,
    const struct scorpi_sco_fixture_layer *a,
    const struct scorpi_sco_fixture_layer *b)
{
	uint8_t page[SCO_FILE_ID_SIZE];
	const struct scorpi_sco_fixture_layer *fixture;
	uint32_t map_page_crc32c;
	int fd;

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	assert(fd >= 0);
	assert(ftruncate(fd, SCO_DATA_AREA_OFFSET) == 0);

	fixture_build_file_id(page, bad_magic, file_major,
	    corrupt_file_id_crc);
	fixture_write_exact(fd, 0, page, sizeof(page));
	fixture_build_superblock(page, a);
	fixture_write_exact(fd, SCO_SUPERBLOCK_A_OFFSET, page, sizeof(page));
	fixture_build_superblock(page, b);
	fixture_write_exact(fd, SCO_SUPERBLOCK_B_OFFSET, page, sizeof(page));

	if (a->base_uri != NULL || b->base_uri != NULL) {
		fixture = a->base_uri != NULL ? a : b;
		fixture_build_base_descriptor(page, fixture);
		fixture_write_exact(fd, SCO_BASE_DESCRIPTOR_OFFSET, page,
		    sizeof(page));
	}
	if (a->write_map_root || b->write_map_root) {
		fixture = a->write_map_root ? a : b;
		map_page_crc32c = 0;
		if (fixture->map_page_present) {
			map_page_crc32c = fixture_build_map_page(page,
			    fixture);
			fixture_write_exact(fd, SCO_MAP_PAGE_OFFSET, page,
			    sizeof(page));
		}
		fixture_build_root_page(page, map_page_crc32c, fixture);
		fixture_write_exact(fd, SCO_MAP_ROOT_OFFSET, page,
		    sizeof(page));
		if (fixture->map_state == SCORPI_SCO_FIXTURE_MAP_PRESENT)
			fixture_write_data_cluster(fd, fixture->data_byte);
	}

	assert(close(fd) == 0);
}

void
scorpi_sco_fixture_write_raw(const char *path, uint64_t size, uint8_t byte)
{
	uint8_t buf[512];
	int fd;

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	assert(fd >= 0);
	assert(ftruncate(fd, (off_t)size) == 0);
	memset(buf, byte, sizeof(buf));
	fixture_write_exact(fd, 0, buf, sizeof(buf));
	assert(close(fd) == 0);
}
