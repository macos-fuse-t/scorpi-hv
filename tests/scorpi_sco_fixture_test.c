/* .sco fixture generator coverage. */

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "scorpi_image.h"
#include "scorpi_sco_fixture.h"

#define	SCO_INCOMPAT_ALLOC_MAP_V1	1
#define	SCO_INCOMPAT_ZERO_DISCARD	2

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
test_fixture_generates_present_cluster(void)
{
	const struct scorpi_image_ops *sco;
	struct scorpi_image_extent extent;
	struct scorpi_image_info info;
	struct scorpi_sco_fixture_layer a, b;
	uint8_t buf[512];
	char path[] = "/tmp/scorpi-sco-fixture-present-XXXXXX";
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	a = (struct scorpi_sco_fixture_layer){
		.present = true,
		.major = 1,
		.virtual_size = SCORPI_SCO_FIXTURE_VIRTUAL_SIZE,
		.incompatible_features = SCO_INCOMPAT_ALLOC_MAP_V1,
		.write_map_root = true,
		.map_page_present = true,
		.map_state = SCORPI_SCO_FIXTURE_MAP_PRESENT,
		.data_byte = 0x93,
	};
	b = (struct scorpi_sco_fixture_layer){ 0 };
	scorpi_sco_fixture_write(path, false, 1, false, &a, &b);

	assert(open_sco(path, &state) == 0);
	sco = scorpi_image_backend_find("sco");
	assert(sco != NULL);
	assert(sco->get_info(state, &info) == 0);
	assert(info.virtual_size == SCORPI_SCO_FIXTURE_VIRTUAL_SIZE);
	assert(info.cluster_size == SCORPI_SCO_FIXTURE_CLUSTER_SIZE);
	assert(sco->map(state, 0, sizeof(buf), &extent) == 0);
	assert(extent.state == SCORPI_IMAGE_EXTENT_PRESENT);
	assert(sco->read(state, buf, 0, sizeof(buf)) == 0);
	assert_buffer_value(buf, sizeof(buf), 0x93);
	assert(sco->close(state) == 0);
	assert(unlink(path) == 0);
}

static void
test_fixture_generates_base_descriptor(void)
{
	const struct scorpi_image_ops *sco;
	struct scorpi_image_info info;
	struct scorpi_sco_fixture_layer a, b;
	char path[] = "/tmp/scorpi-sco-fixture-base-XXXXXX";
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	a = (struct scorpi_sco_fixture_layer){
		.present = true,
		.major = 1,
		.virtual_size = SCORPI_SCO_FIXTURE_VIRTUAL_SIZE,
		.incompatible_features = SCO_INCOMPAT_ALLOC_MAP_V1,
		.base_uri = "file:base.raw",
		.has_base_uuid = true,
		.has_base_digest = true,
	};
	b = (struct scorpi_sco_fixture_layer){ 0 };
	scorpi_sco_fixture_write(path, false, 1, false, &a, &b);

	assert(open_sco(path, &state) == 0);
	sco = scorpi_image_backend_find("sco");
	assert(sco != NULL);
	memset(&info, 0, sizeof(info));
	assert(sco->get_info(state, &info) == 0);
	assert(info.has_base);
	assert(strcmp(info.base_uri, "file:base.raw") == 0);
	assert(info.has_base_uuid);
	assert(info.has_base_digest);
	free(info.base_uri);
	assert(sco->close(state) == 0);
	assert(unlink(path) == 0);
}

static void
test_fixture_generates_corrupt_variants(void)
{
	struct scorpi_sco_fixture_layer a, b;
	char path[] = "/tmp/scorpi-sco-fixture-corrupt-XXXXXX";
	void *state;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	a = (struct scorpi_sco_fixture_layer){
		.present = true,
		.major = 1,
		.virtual_size = SCORPI_SCO_FIXTURE_VIRTUAL_SIZE,
		.incompatible_features = SCO_INCOMPAT_ALLOC_MAP_V1 |
		    SCO_INCOMPAT_ZERO_DISCARD,
		.write_map_root = true,
		.map_page_present = true,
		.map_state = SCORPI_SCO_FIXTURE_MAP_ZERO,
		.corrupt_map_page_crc = true,
	};
	b = (struct scorpi_sco_fixture_layer){ 0 };
	scorpi_sco_fixture_write(path, false, 1, false, &a, &b);

	assert(open_sco(path, &state) == 0);
	assert(scorpi_image_backend_find("sco")->map(state, 0, 512,
	    &(struct scorpi_image_extent){ 0 }) != 0);
	assert(scorpi_image_backend_find("sco")->close(state) == 0);
	scorpi_sco_fixture_write(path, false, 1, true, &a, &b);
	assert(open_sco(path, &state) != 0);
	assert(unlink(path) == 0);
}

int
main(void)
{
	test_fixture_generates_present_cluster();
	test_fixture_generates_base_descriptor();
	test_fixture_generates_corrupt_variants();
	return (0);
}
