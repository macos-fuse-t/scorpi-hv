/* .sco test fixture generator. */

#ifndef _SCORPI_SCO_FIXTURE_H_
#define	_SCORPI_SCO_FIXTURE_H_

#include <stdbool.h>
#include <stdint.h>

#define	SCORPI_SCO_FIXTURE_VIRTUAL_SIZE		(128ULL * 1024 * 1024)
#define	SCORPI_SCO_FIXTURE_CLUSTER_SIZE		0x40000U

enum scorpi_sco_fixture_map_state {
	SCORPI_SCO_FIXTURE_MAP_ABSENT = 0,
	SCORPI_SCO_FIXTURE_MAP_PRESENT = 1,
	SCORPI_SCO_FIXTURE_MAP_ZERO = 2,
	SCORPI_SCO_FIXTURE_MAP_DISCARDED = 3,
};

struct scorpi_sco_fixture_layer {
	bool present;
	bool corrupt_superblock_crc;
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
	enum scorpi_sco_fixture_map_state map_state;
	uint8_t data_byte;
};

extern const uint8_t scorpi_sco_fixture_uuid[16];

void	scorpi_sco_fixture_write(const char *path, bool bad_magic,
	    uint16_t file_major, bool corrupt_file_id_crc,
	    const struct scorpi_sco_fixture_layer *a,
	    const struct scorpi_sco_fixture_layer *b);
void	scorpi_sco_fixture_write_raw(const char *path, uint64_t size,
	    uint8_t byte);

#endif /* _SCORPI_SCO_FIXTURE_H_ */
