/* .sco fixed-table format coverage. */

#include <sys/errno.h>
#include <sys/stat.h>

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <support/endian.h>

#include "scorpi_crc32c.h"
#include "scorpi_image.h"
#include "scorpi_image_chain.h"
#include "scorpi_image_sco.h"
#include "scorpi_image_sco_format.h"

#define	TEST_VIRTUAL_SIZE		(128ULL * 1024 * 1024)
#define	TEST_CLUSTER_SIZE		0x40000U
#define	TEST_TABLE_OFFSET		SCO_METADATA_AREA_OFFSET
#define	TEST_BASE_DESCRIPTOR_OFFSET	SCO_METADATA_AREA_OFFSET
#define	TEST_BASE_TABLE_OFFSET \
	(SCO_METADATA_AREA_OFFSET + SCO_METADATA_PAGE_SIZE)
#define	TEST_DATA_AREA_OFFSET		0x40000ULL
#define	TEST_SLOT_A			0
#define	TEST_SLOT_B			1

static const uint8_t test_uuid[16] = {
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

struct concurrent_sco_write_arg {
	const struct scorpi_image_ops *sco;
	void *state;
	uint64_t offset;
	uint8_t byte;
	int error;
};

static uint64_t
align_up_u64(uint64_t value, uint64_t alignment)
{
	return ((value + alignment - 1) & ~(alignment - 1));
}

static uint64_t
cluster_count(uint64_t virtual_size)
{
	return ((virtual_size + TEST_CLUSTER_SIZE - 1) / TEST_CLUSTER_SIZE);
}

static uint32_t
table_length(uint64_t virtual_size)
{
	return ((uint32_t)align_up_u64(cluster_count(virtual_size) *
	    SCO_TABLE_SLOT_SIZE, SCO_METADATA_PAGE_SIZE));
}

static void
write_exact_at(int fd, uint64_t offset, const void *buf, size_t len)
{
	assert(pwrite(fd, buf, len, (off_t)offset) == (ssize_t)len);
}

static uint64_t
file_size_of(const char *path)
{
	struct stat sb;

	assert(stat(path, &sb) == 0);
	return ((uint64_t)sb.st_size);
}

static void
assert_buffer_value(const uint8_t *buf, size_t len, uint8_t expected)
{
	size_t i;

	for (i = 0; i < len; i++)
		assert(buf[i] == expected);
}

static void
build_file_id(uint8_t buf[SCO_FILE_ID_SIZE], bool bad_magic,
    uint16_t major, bool corrupt_crc)
{
	uint32_t crc;

	memset(buf, 0, SCO_FILE_ID_SIZE);
	memcpy(buf, bad_magic ? "BADIMG\0\0" : SCO_MAGIC, SCO_MAGIC_SIZE);
	le16enc(buf + 0x0008, major);
	le32enc(buf + 0x000c, SCO_FILE_ID_SIZE);
	memcpy(buf + 0x0010, test_uuid, sizeof(test_uuid));
	le32enc(buf + 0x0020, SCO_CHECKSUM_CRC32C);
	crc = scorpi_crc32c(buf, SCO_FILE_ID_SIZE);
	if (corrupt_crc)
		crc ^= 0xffffffffU;
	le32enc(buf + 0x0024, crc);
}

static void
build_superblock(uint8_t buf[SCO_SUPERBLOCK_SIZE], uint16_t major,
    uint64_t generation, uint64_t virtual_size, const char *base_uri,
    bool sealed)
{
	uint64_t table_offset, data_area_offset;
	uint32_t tlen, crc;

	tlen = table_length(virtual_size);
	table_offset = base_uri == NULL ? TEST_TABLE_OFFSET :
	    TEST_BASE_TABLE_OFFSET;
	data_area_offset = align_up_u64(table_offset +
	    2ULL * tlen, TEST_CLUSTER_SIZE);
	if (data_area_offset < TEST_DATA_AREA_OFFSET)
		data_area_offset = TEST_DATA_AREA_OFFSET;

	memset(buf, 0, SCO_SUPERBLOCK_SIZE);
	memcpy(buf, SCO_MAGIC, SCO_MAGIC_SIZE);
	le16enc(buf + 0x0008, major);
	le32enc(buf + 0x000c, SCO_SUPERBLOCK_SIZE);
	le32enc(buf + 0x0010, SCO_CHECKSUM_CRC32C);
	le64enc(buf + 0x0018, generation);
	le64enc(buf + 0x0020, virtual_size);
	le32enc(buf + 0x0028, 512);
	le32enc(buf + 0x002c, 4096);
	le32enc(buf + 0x0030, TEST_CLUSTER_SIZE);
	le64enc(buf + 0x0038, cluster_count(virtual_size));
	le64enc(buf + 0x0040, SCO_METADATA_AREA_OFFSET);
	le64enc(buf + 0x0048, data_area_offset);
	if (base_uri != NULL) {
		le64enc(buf + 0x0050, TEST_BASE_DESCRIPTOR_OFFSET);
		le32enc(buf + 0x0058, SCO_METADATA_PAGE_SIZE);
	}
	le64enc(buf + 0x0060, table_offset);
	le32enc(buf + 0x0068, tlen);
	le32enc(buf + 0x006c, SCO_TABLE_SLOT_SIZE);
	le32enc(buf + 0x0070, SCO_INCOMPAT_FIXED_TABLE_V2);
	if (sealed)
		le32enc(buf + 0x0074, SCO_RO_COMPAT_SEALED);
	memcpy(buf + 0x0080, test_uuid, sizeof(test_uuid));
	crc = scorpi_crc32c(buf, SCO_SUPERBLOCK_SIZE);
	le32enc(buf + 0x0014, crc);
}

static void
build_base_descriptor(uint8_t buf[SCO_METADATA_PAGE_SIZE],
    const char *base_uri)
{
	uint32_t crc;
	size_t uri_len;

	memset(buf, 0, SCO_METADATA_PAGE_SIZE);
	uri_len = strlen(base_uri);
	le32enc(buf + 0x0000, SCO_METADATA_PAGE_SIZE);
	le32enc(buf + 0x0004, SCO_CHECKSUM_CRC32C);
	le32enc(buf + 0x0010, (uint32_t)uri_len);
	memcpy(buf + 0x0050, base_uri, uri_len);
	crc = scorpi_crc32c(buf, SCO_METADATA_PAGE_SIZE);
	le32enc(buf + 0x0008, crc);
}

static uint32_t
slot_crc32c(uint64_t cluster_index, uint8_t slot_index,
    const uint8_t slot[SCO_TABLE_SLOT_SIZE])
{
	uint8_t context[16 + 8 + 4 + SCO_TABLE_SLOT_SIZE];
	uint8_t slot_copy[SCO_TABLE_SLOT_SIZE];

	memcpy(slot_copy, slot, sizeof(slot_copy));
	memset(slot_copy + 0x0008, 0, 4);
	memcpy(context, test_uuid, sizeof(test_uuid));
	le64enc(context + 16, cluster_index);
	le32enc(context + 24, slot_index);
	memcpy(context + 28, slot_copy, sizeof(slot_copy));
	return (scorpi_crc32c(context, sizeof(context)));
}

static void
build_slot(uint8_t slot[SCO_TABLE_SLOT_SIZE], uint64_t cluster_index,
    uint8_t slot_index, uint32_t generation, uint8_t state,
    uint32_t physical_cluster, bool corrupt_crc)
{
	uint32_t crc;

	memset(slot, 0, SCO_TABLE_SLOT_SIZE);
	le32enc(slot + 0x0000, generation);
	le32enc(slot + 0x0004, physical_cluster);
	slot[0x000c] = state;
	crc = slot_crc32c(cluster_index, slot_index, slot);
	if (corrupt_crc)
		crc ^= 0xffffffffU;
	le32enc(slot + 0x0008, crc);
}

static uint64_t
slot_offset(uint64_t table_offset, uint64_t cluster_index, uint8_t slot_index)
{
	return (table_offset + cluster_index * SCO_TABLE_ENTRY_SIZE +
	    slot_index * SCO_TABLE_SLOT_SIZE);
}

static void
write_data_cluster(int fd, uint64_t cluster_index, uint8_t byte)
{
	uint8_t buf[4096];
	uint64_t data_offset;

	data_offset = TEST_DATA_AREA_OFFSET + cluster_index * TEST_CLUSTER_SIZE;
	assert(ftruncate(fd, (off_t)(data_offset + TEST_CLUSTER_SIZE)) == 0);
	memset(buf, byte, sizeof(buf));
	write_exact_at(fd, data_offset, buf, sizeof(buf));
}

static void
write_fixture(const char *path, uint8_t state, uint8_t data_byte,
    const char *base_uri, bool corrupt_file_id_crc, bool corrupt_slot_crc,
    bool sealed)
{
	uint8_t page[SCO_FILE_ID_SIZE];
	uint64_t table_offset, data_area_offset;
	uint32_t tlen;
	int fd;

	tlen = table_length(TEST_VIRTUAL_SIZE);
	table_offset = base_uri == NULL ? TEST_TABLE_OFFSET :
	    TEST_BASE_TABLE_OFFSET;
	data_area_offset = align_up_u64(table_offset + 2ULL * tlen,
	    TEST_CLUSTER_SIZE);
	if (data_area_offset < TEST_DATA_AREA_OFFSET)
		data_area_offset = TEST_DATA_AREA_OFFSET;

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	assert(fd >= 0);
	assert(ftruncate(fd, (off_t)data_area_offset) == 0);
	build_file_id(page, false, SCO_FORMAT_MAJOR, corrupt_file_id_crc);
	write_exact_at(fd, 0, page, sizeof(page));
	build_superblock(page, SCO_FORMAT_MAJOR, 1, TEST_VIRTUAL_SIZE,
	    base_uri, sealed);
	write_exact_at(fd, SCO_SUPERBLOCK_A_OFFSET, page, sizeof(page));
	memset(page, 0, sizeof(page));
	write_exact_at(fd, SCO_SUPERBLOCK_B_OFFSET, page, sizeof(page));
	if (base_uri != NULL) {
		build_base_descriptor(page, base_uri);
		write_exact_at(fd, TEST_BASE_DESCRIPTOR_OFFSET, page,
		    SCO_METADATA_PAGE_SIZE);
	}
	if (state != SCO_MAP_STATE_ABSENT) {
		build_slot(page, 0, 0, 1, state,
		    state == SCO_MAP_STATE_PRESENT ?
		    (uint32_t)(TEST_DATA_AREA_OFFSET / TEST_CLUSTER_SIZE) : 0,
		    corrupt_slot_crc);
		write_exact_at(fd, slot_offset(table_offset, 0, 0), page,
		    SCO_TABLE_SLOT_SIZE);
		if (state == SCO_MAP_STATE_PRESENT)
			write_data_cluster(fd, 0, data_byte);
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

static int
open_sco_rw(const char *path, void **statep)
{
	const struct scorpi_image_ops *sco;
	int error, fd;

	sco = scorpi_image_backend_find("sco");
	assert(sco != NULL);
	fd = open(path, O_RDWR);
	assert(fd >= 0);
	*statep = NULL;
	error = sco->open(path, fd, false, statep);
	if (error != 0)
		assert(close(fd) == 0);
	return (error);
}

static void
write_raw_file(const char *path, uint64_t size, uint8_t byte)
{
	uint8_t buf[4096];
	int fd;

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	assert(fd >= 0);
	assert(ftruncate(fd, (off_t)size) == 0);
	memset(buf, byte, sizeof(buf));
	write_exact_at(fd, 0, buf, sizeof(buf));
	assert(close(fd) == 0);
}

static void
write_raw_range_value(const char *path, uint64_t offset, size_t len,
    uint8_t byte)
{
	uint8_t buf[4096];
	size_t n;
	int fd;

	fd = open(path, O_RDWR);
	assert(fd >= 0);
	memset(buf, byte, sizeof(buf));
	while (len > 0) {
		n = len > sizeof(buf) ? sizeof(buf) : len;
		write_exact_at(fd, offset, buf, n);
		offset += n;
		len -= n;
	}
	assert(close(fd) == 0);
}

static void
test_crc32c_known_vector(void)
{
	static const char vector[] = "123456789";

	assert(scorpi_crc32c(vector, strlen(vector)) == 0xe3069283U);
}

static void
test_valid_sco_opens(void)
{
	const struct scorpi_image_ops *sco;
	struct scorpi_image_extent extent;
	struct scorpi_image_info info;
	char path[] = "/tmp/scorpi-sco-valid-XXXXXX";
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	write_fixture(path, SCO_MAP_STATE_ABSENT, 0, NULL, false, false,
	    false);
	assert(open_sco(path, &state) == 0);
	sco = scorpi_image_backend_find("sco");
	memset(&info, 0, sizeof(info));
	assert(sco->get_info(state, &info) == 0);
	assert(info.virtual_size == TEST_VIRTUAL_SIZE);
	assert(info.cluster_size == TEST_CLUSTER_SIZE);
	assert(info.has_image_uuid);
	assert(!info.has_base);
	assert(sco->map(state, 0, 512, &extent) == 0);
	assert(extent.state == SCORPI_IMAGE_EXTENT_ABSENT);
	assert(sco->close(state) == 0);
	assert(unlink(path) == 0);
}

static void
test_bad_magic_and_version_rejected(void)
{
	uint8_t page[SCO_FILE_ID_SIZE];
	char path[] = "/tmp/scorpi-sco-bad-XXXXXX";
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	build_file_id(page, true, SCO_FORMAT_MAJOR, false);
	write_exact_at(fd, 0, page, sizeof(page));
	assert(ftruncate(fd, TEST_DATA_AREA_OFFSET) == 0);
	assert(close(fd) == 0);
	assert(open_sco(path, &state) != 0);

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	assert(fd >= 0);
	build_file_id(page, false, SCO_FORMAT_MAJOR + 1, false);
	write_exact_at(fd, 0, page, sizeof(page));
	assert(ftruncate(fd, TEST_DATA_AREA_OFFSET) == 0);
	assert(close(fd) == 0);
	assert(open_sco(path, &state) != 0);
	assert(unlink(path) == 0);
}

static void
test_present_zero_and_discarded_states(void)
{
	const struct scorpi_image_ops *sco;
	struct scorpi_image_extent extent;
	uint8_t buf[512];
	char path[] = "/tmp/scorpi-sco-states-XXXXXX";
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	write_fixture(path, SCO_MAP_STATE_PRESENT, 0x93, NULL, false, false,
	    false);
	assert(open_sco(path, &state) == 0);
	sco = scorpi_image_backend_find("sco");
	assert(sco->map(state, 0, sizeof(buf), &extent) == 0);
	assert(extent.state == SCORPI_IMAGE_EXTENT_PRESENT);
	assert(sco->read(state, buf, 0, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 0x93);
	assert(sco->close(state) == 0);

	write_fixture(path, SCO_MAP_STATE_ZERO, 0, NULL, false, false, false);
	assert(open_sco(path, &state) == 0);
	assert(sco->read(state, buf, 0, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 0);
	assert(sco->close(state) == 0);

	write_fixture(path, SCO_MAP_STATE_DISCARDED, 0, NULL, false, false,
	    false);
	assert(open_sco(path, &state) == 0);
	assert(sco->read(state, buf, 0, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 0);
	assert(sco->close(state) == 0);
	assert(unlink(path) == 0);
}

static void
test_full_cluster_write_persists_after_reopen(void)
{
	const struct scorpi_image_ops *sco;
	uint8_t *cluster, buf[512];
	char path[] = "/tmp/scorpi-sco-write-XXXXXX";
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	write_fixture(path, SCO_MAP_STATE_ABSENT, 0, NULL, false, false,
	    false);
	cluster = malloc(TEST_CLUSTER_SIZE);
	assert(cluster != NULL);
	memset(cluster, 0x4f, TEST_CLUSTER_SIZE);
	assert(open_sco_rw(path, &state) == 0);
	sco = scorpi_image_backend_find("sco");
	assert(sco->write(state, cluster, 0, TEST_CLUSTER_SIZE) == 0);
	assert(sco->flush(state) == 0);
	assert(sco->close(state) == 0);

	assert(open_sco(path, &state) == 0);
	memset(buf, 0, sizeof(buf));
	assert(sco->read(state, buf, 0, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 0x4f);
	assert(sco->close(state) == 0);
	free(cluster);
	assert(unlink(path) == 0);
}

static void
test_two_slot_table_recovers_from_torn_newest_slot(void)
{
	const struct scorpi_image_ops *sco;
	uint8_t *cluster, buf[512], slot[SCO_TABLE_SLOT_SIZE];
	char path[] = "/tmp/scorpi-sco-slot-recover-XXXXXX";
	uint64_t size_before, size_after;
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	write_fixture(path, SCO_MAP_STATE_ABSENT, 0, NULL, false, false,
	    false);
	cluster = malloc(TEST_CLUSTER_SIZE);
	assert(cluster != NULL);
	sco = scorpi_image_backend_find("sco");
	assert(open_sco_rw(path, &state) == 0);
	memset(cluster, 0x11, TEST_CLUSTER_SIZE);
	assert(sco->write(state, cluster, 0, TEST_CLUSTER_SIZE) == 0);
	assert(sco->close(state) == 0);

	fd = open(path, O_RDWR);
	assert(fd >= 0);
	size_before = file_size_of(path);
	build_slot(slot, 0, TEST_SLOT_B, 2, SCO_MAP_STATE_ZERO, 0, true);
	write_exact_at(fd, slot_offset(TEST_TABLE_OFFSET, 0, TEST_SLOT_B),
	    slot, sizeof(slot));
	assert(close(fd) == 0);

	assert(open_sco(path, &state) == 0);
	memset(buf, 0, sizeof(buf));
	assert(sco->read(state, buf, 0, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 0x11);
	assert(sco->close(state) == 0);

	assert(open_sco_rw(path, &state) == 0);
	memset(cluster, 0x33, TEST_CLUSTER_SIZE);
	assert(sco->write(state, cluster, TEST_CLUSTER_SIZE,
	    TEST_CLUSTER_SIZE) == 0);
	assert(sco->close(state) == 0);
	size_after = file_size_of(path);
	assert(size_after > size_before);
	free(cluster);
	assert(unlink(path) == 0);
}

static void
test_reuses_freed_cluster_after_reopen(void)
{
	const struct scorpi_image_ops *sco;
	uint8_t *cluster, buf[512];
	char path[] = "/tmp/scorpi-sco-reuse-XXXXXX";
	uint64_t size_after_first_write, size_after_reuse;
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	write_fixture(path, SCO_MAP_STATE_ABSENT, 0, NULL, false, false,
	    false);
	cluster = malloc(TEST_CLUSTER_SIZE);
	assert(cluster != NULL);
	sco = scorpi_image_backend_find("sco");

	assert(open_sco_rw(path, &state) == 0);
	memset(cluster, 0x51, TEST_CLUSTER_SIZE);
	assert(sco->write(state, cluster, 0, TEST_CLUSTER_SIZE) == 0);
	size_after_first_write = file_size_of(path);
	memset(cluster, 0, TEST_CLUSTER_SIZE);
	assert(sco->write(state, cluster, 0, TEST_CLUSTER_SIZE) == 0);
	assert(sco->close(state) == 0);

	assert(open_sco_rw(path, &state) == 0);
	memset(cluster, 0x62, TEST_CLUSTER_SIZE);
	assert(sco->write(state, cluster, TEST_CLUSTER_SIZE,
	    TEST_CLUSTER_SIZE) == 0);
	assert(sco->close(state) == 0);
	size_after_reuse = file_size_of(path);
	assert(size_after_reuse == size_after_first_write);

	assert(open_sco(path, &state) == 0);
	memset(buf, 0xff, sizeof(buf));
	assert(sco->read(state, buf, 0, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 0);
	memset(buf, 0, sizeof(buf));
	assert(sco->read(state, buf, TEST_CLUSTER_SIZE, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 0x62);
	assert(sco->close(state) == 0);
	free(cluster);
	assert(unlink(path) == 0);
}

static void
test_full_zero_write_maps_zero(void)
{
	const struct scorpi_image_ops *sco;
	uint8_t *cluster, buf[512];
	char path[] = "/tmp/scorpi-sco-zero-write-XXXXXX";
	uint64_t size_before, size_after;
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	write_fixture(path, SCO_MAP_STATE_PRESENT, 0x76, NULL, false, false,
	    false);
	size_before = file_size_of(path);
	cluster = calloc(1, TEST_CLUSTER_SIZE);
	assert(cluster != NULL);
	assert(open_sco_rw(path, &state) == 0);
	sco = scorpi_image_backend_find("sco");
	assert(sco->write(state, cluster, 0, TEST_CLUSTER_SIZE) == 0);
	assert(sco->close(state) == 0);
	size_after = file_size_of(path);
	assert(size_after == size_before);

	assert(open_sco(path, &state) == 0);
	assert(sco->read(state, buf, 0, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 0);
	assert(sco->close(state) == 0);
	free(cluster);
	assert(unlink(path) == 0);
}

static void *
concurrent_sco_write_thread(void *argp)
{
	struct concurrent_sco_write_arg *arg;
	uint8_t *cluster;

	arg = argp;
	cluster = malloc(TEST_CLUSTER_SIZE);
	assert(cluster != NULL);
	memset(cluster, arg->byte, TEST_CLUSTER_SIZE);
	arg->error = arg->sco->write(arg->state, cluster, arg->offset,
	    TEST_CLUSTER_SIZE);
	free(cluster);
	return (NULL);
}

static void
test_concurrent_sco_writes_preserve_table_updates(void)
{
	enum { THREAD_COUNT = 4 };
	const struct scorpi_image_ops *sco;
	struct concurrent_sco_write_arg args[THREAD_COUNT];
	pthread_t threads[THREAD_COUNT];
	uint8_t buf[512];
	char path[] = "/tmp/scorpi-sco-concurrent-XXXXXX";
	void *state;
	size_t i;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	write_fixture(path, SCO_MAP_STATE_ABSENT, 0, NULL, false, false,
	    false);
	assert(open_sco_rw(path, &state) == 0);
	sco = scorpi_image_backend_find("sco");
	for (i = 0; i < THREAD_COUNT; i++) {
		args[i] = (struct concurrent_sco_write_arg){
			.sco = sco,
			.state = state,
			.offset = i * TEST_CLUSTER_SIZE,
			.byte = (uint8_t)(0x40 + i),
		};
		assert(pthread_create(&threads[i], NULL,
		    concurrent_sco_write_thread, &args[i]) == 0);
	}
	for (i = 0; i < THREAD_COUNT; i++) {
		assert(pthread_join(threads[i], NULL) == 0);
		assert(args[i].error == 0);
	}
	for (i = 0; i < THREAD_COUNT; i++) {
		memset(buf, 0, sizeof(buf));
		assert(sco->read(state, buf, i * TEST_CLUSTER_SIZE,
		    sizeof(buf)) == 0);
		assert_buffer_value(buf, sizeof(buf), (uint8_t)(0x40 + i));
	}
	assert(sco->close(state) == 0);
	assert(unlink(path) == 0);
}

static void
test_partial_chain_write_preserves_base_bytes(void)
{
	struct scorpi_image_chain_open_options options;
	struct scorpi_image_chain *chain;
	uint8_t writebuf[16], readbuf[8192];
	char child[] = "/tmp/scorpi-sco-chain-child-XXXXXX";
	char base[] = "/tmp/scorpi-sco-chain-base-XXXXXX";
	char uri[128];
	int child_fd, fd;

	child_fd = mkstemp(child);
	assert(child_fd >= 0);
	assert(close(child_fd) == 0);
	fd = mkstemp(base);
	assert(fd >= 0);
	assert(close(fd) == 0);
	assert(unlink(base) == 0);
	write_raw_file(base, TEST_VIRTUAL_SIZE, 0x45);
	write_raw_range_value(base, 0, TEST_CLUSTER_SIZE, 0x45);

	snprintf(uri, sizeof(uri), "file:%s", strrchr(base, '/') + 1);
	write_fixture(child, SCO_MAP_STATE_ABSENT, 0, uri, false, false,
	    false);
	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = true,
	};
	child_fd = open(child, O_RDWR);
	assert(child_fd >= 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single(child, child_fd, false, &options,
	    &chain) == 0);
	memset(writebuf, 0xee, sizeof(writebuf));
	assert(scorpi_image_chain_write(chain, writebuf, 4096,
	    sizeof(writebuf)) == 0);
	assert(scorpi_image_chain_flush(chain) == 0);
	assert(scorpi_image_chain_close(chain) == 0);

	child_fd = open(child, O_RDONLY);
	assert(child_fd >= 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single(child, child_fd, true, &options,
	    &chain) == 0);
	memset(readbuf, 0, sizeof(readbuf));
	assert(scorpi_image_chain_read(chain, readbuf, 0, sizeof(readbuf)) == 0);
	assert_buffer_value(readbuf, 4096, 0x45);
	assert_buffer_value(readbuf + 4096, sizeof(writebuf), 0xee);
	assert_buffer_value(readbuf + 4096 + sizeof(writebuf),
	    sizeof(readbuf) - 4096 - sizeof(writebuf), 0x45);
	assert(scorpi_image_chain_close(chain) == 0);
	assert(unlink(child) == 0);
	assert(unlink(base) == 0);
}

static void
test_direct_partial_absent_write_with_base_rejected(void)
{
	const struct scorpi_image_ops *sco;
	uint8_t writebuf[16];
	char child[] = "/tmp/scorpi-sco-direct-child-XXXXXX";
	char base[] = "/tmp/scorpi-sco-direct-base-XXXXXX";
	char uri[128];
	void *state;
	int child_fd, fd;

	child_fd = mkstemp(child);
	assert(child_fd >= 0);
	assert(close(child_fd) == 0);
	fd = mkstemp(base);
	assert(fd >= 0);
	assert(close(fd) == 0);
	assert(unlink(base) == 0);
	write_raw_file(base, TEST_VIRTUAL_SIZE, 0x45);

	snprintf(uri, sizeof(uri), "file:%s", strrchr(base, '/') + 1);
	write_fixture(child, SCO_MAP_STATE_ABSENT, 0, uri, false, false,
	    false);
	assert(open_sco_rw(child, &state) == 0);
	sco = scorpi_image_backend_find("sco");
	memset(writebuf, 0xee, sizeof(writebuf));
	assert(sco->write(state, writebuf, 4096, sizeof(writebuf)) ==
	    EOPNOTSUPP);
	assert(sco->discard(state, 4096, sizeof(writebuf)) == EOPNOTSUPP);
	assert(sco->close(state) == 0);
	assert(unlink(child) == 0);
	assert(unlink(base) == 0);
}

static void
test_partial_chain_discard_preserves_base_bytes(void)
{
	struct scorpi_image_chain_open_options options;
	struct scorpi_image_chain *chain;
	uint8_t readbuf[8192];
	char child[] = "/tmp/scorpi-sco-chain-discard-child-XXXXXX";
	char base[] = "/tmp/scorpi-sco-chain-discard-base-XXXXXX";
	char uri[128];
	int child_fd, fd;

	child_fd = mkstemp(child);
	assert(child_fd >= 0);
	assert(close(child_fd) == 0);
	fd = mkstemp(base);
	assert(fd >= 0);
	assert(close(fd) == 0);
	assert(unlink(base) == 0);
	write_raw_file(base, TEST_VIRTUAL_SIZE, 0x45);
	write_raw_range_value(base, 0, TEST_CLUSTER_SIZE, 0x45);

	snprintf(uri, sizeof(uri), "file:%s", strrchr(base, '/') + 1);
	write_fixture(child, SCO_MAP_STATE_ABSENT, 0, uri, false, false,
	    false);
	options = (struct scorpi_image_chain_open_options){
		.raw_fallback = true,
	};
	child_fd = open(child, O_RDWR);
	assert(child_fd >= 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single(child, child_fd, false, &options,
	    &chain) == 0);
	assert(scorpi_image_chain_discard(chain, 4096, 16) == 0);
	assert(scorpi_image_chain_flush(chain) == 0);
	assert(scorpi_image_chain_close(chain) == 0);

	child_fd = open(child, O_RDONLY);
	assert(child_fd >= 0);
	chain = NULL;
	assert(scorpi_image_chain_open_single(child, child_fd, true, &options,
	    &chain) == 0);
	memset(readbuf, 0, sizeof(readbuf));
	assert(scorpi_image_chain_read(chain, readbuf, 0, sizeof(readbuf)) == 0);
	assert_buffer_value(readbuf, 4096, 0x45);
	assert_buffer_value(readbuf + 4096, 16, 0);
	assert_buffer_value(readbuf + 4096 + 16,
	    sizeof(readbuf) - 4096 - 16, 0x45);
	assert(scorpi_image_chain_close(chain) == 0);
	assert(unlink(child) == 0);
	assert(unlink(base) == 0);
}

static void
test_readonly_and_sealed_sco_reject_write(void)
{
	const struct scorpi_image_ops *sco;
	uint8_t cluster[512];
	char readonly_path[] = "/tmp/scorpi-sco-readonly-XXXXXX";
	char sealed_path[] = "/tmp/scorpi-sco-sealed-XXXXXX";
	void *state;
	int fd;

	fd = mkstemp(readonly_path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	write_fixture(readonly_path, SCO_MAP_STATE_ABSENT, 0, NULL, false,
	    false, false);
	assert(open_sco(readonly_path, &state) == 0);
	sco = scorpi_image_backend_find("sco");
	memset(cluster, 0x4d, sizeof(cluster));
	assert(sco->write(state, cluster, 0, sizeof(cluster)) == EROFS);
	assert(sco->close(state) == 0);
	assert(unlink(readonly_path) == 0);

	fd = mkstemp(sealed_path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	write_fixture(sealed_path, SCO_MAP_STATE_ABSENT, 0, NULL, false,
	    false, true);
	state = NULL;
	assert(open_sco_rw(sealed_path, &state) == EROFS);
	assert(state == NULL);
	assert(open_sco(sealed_path, &state) == 0);
	assert(sco->write(state, cluster, 0, sizeof(cluster)) == EROFS);
	assert(sco->close(state) == 0);
	assert(unlink(sealed_path) == 0);
}

int
main(void)
{
	test_crc32c_known_vector();
	test_valid_sco_opens();
	test_bad_magic_and_version_rejected();
	test_present_zero_and_discarded_states();
	test_full_cluster_write_persists_after_reopen();
	test_two_slot_table_recovers_from_torn_newest_slot();
	test_reuses_freed_cluster_after_reopen();
	test_full_zero_write_maps_zero();
	test_concurrent_sco_writes_preserve_table_updates();
	test_partial_chain_write_preserves_base_bytes();
	test_direct_partial_absent_write_with_base_rejected();
	test_partial_chain_discard_preserves_base_bytes();
	test_readonly_and_sealed_sco_reject_write();
	return (0);
}
