# Scorpi `.sco` V1 Image Format

## Purpose

This document specifies the fixed V1 on-disk layout for Scorpi native overlay
images.

The file extension is:

```text
.sco
```

The extension is advisory. Format detection uses the file identifier magic.

## Global Rules

- All multi-byte integers are little-endian.
- All offsets are byte offsets from the start of the file.
- All reserved fields must be written as zero.
- Readers must reject non-zero reserved fields unless this document explicitly
  says otherwise.
- All fixed structs are packed on disk. Implementations should parse fields by
  explicit offsets, not by directly casting host C structs.
- The minimum valid file size is `0x00010000`.
- V1 supports only `format_major = 1` and `format_minor = 0`.
- Unknown incompatible feature bits must reject the image.
- Unknown readonly-compatible feature bits may open only when the image is
  opened readonly.
- Unknown compatible feature bits may be ignored.

## Constants

```text
SCO_V1_MAGIC                  = "SCOIMG\0\0"
SCO_V1_FORMAT_MAJOR           = 1
SCO_V1_FORMAT_MINOR           = 0
SCO_V1_FILE_ID_OFFSET         = 0x00000000
SCO_V1_FILE_ID_SIZE           = 0x00001000
SCO_V1_SUPERBLOCK_A_OFFSET    = 0x00001000
SCO_V1_SUPERBLOCK_B_OFFSET    = 0x00002000
SCO_V1_SUPERBLOCK_SIZE        = 0x00001000
SCO_V1_RESERVED_AREA_OFFSET   = 0x00003000
SCO_V1_RESERVED_AREA_SIZE     = 0x0000d000
SCO_V1_METADATA_AREA_OFFSET   = 0x00010000
SCO_V1_METADATA_PAGE_SIZE     = 0x00001000
SCO_V1_DEFAULT_CLUSTER_SIZE   = 0x00040000
SCO_V1_MIN_CLUSTER_SIZE       = 0x00010000
SCO_V1_MAX_CLUSTER_SIZE       = 0x00400000
```

## Fixed File Layout

```text
0x00000000  File identifier block, 4096 bytes
0x00001000  Superblock A, 4096 bytes
0x00002000  Superblock B, 4096 bytes
0x00003000  Reserved header area, 53248 bytes
0x00010000  Metadata allocation area starts
             - base descriptor pages
             - map root pages
             - map pages
             - future metadata pages
data_area_offset  Cluster data allocation area starts
```

`data_area_offset` is stored in the selected superblock. It must be:

- greater than or equal to `0x00010000`
- aligned to `cluster_size`
- greater than every metadata page referenced by the selected superblock

V1 writers should allocate metadata pages before `data_area_offset` and cluster
data at or after `data_area_offset`. V1 readers must reject any map entry whose
`physical_offset` is below `data_area_offset`.

## Checksums

V1 uses CRC-32C for metadata checksums.

Checksum fields are calculated with the checksum field itself treated as zero.
The covered byte range is specified per structure.

```text
SCO_V1_CHECKSUM_NONE     = 0
SCO_V1_CHECKSUM_CRC32C   = 1
```

V1 writers must use CRC-32C for:

- file identifier block
- superblocks
- base descriptor
- map root pages
- map pages

V1 readers must reject metadata with unsupported checksum types or invalid
checksums.

## File Identifier Block

Offset: `0x00000000`

Size: `4096`

The file identifier block is written at image creation and never rewritten.

```text
Offset  Size  Field
0x0000  8     magic = "SCOIMG\0\0"
0x0008  2     format_major = 1
0x000a  2     format_minor = 0
0x000c  4     block_size = 4096
0x0010  16    image_uuid
0x0020  4     checksum_type = 1
0x0024  4     block_crc32c
0x0028  4056  reserved
```

`block_crc32c` covers bytes `0x0000..0x0fff` of the file identifier block with
bytes `0x0024..0x0027` treated as zero.

The `image_uuid` in the file identifier block must match the `image_uuid` in
the selected superblock.

## Superblock

Offsets:

- Superblock A: `0x00001000`
- Superblock B: `0x00002000`

Size: `4096`

```text
Offset  Size  Field
0x0000  8     magic = "SCOIMG\0\0"
0x0008  2     format_major = 1
0x000a  2     format_minor = 0
0x000c  4     superblock_size = 4096
0x0010  4     checksum_type = 1
0x0014  4     superblock_crc32c
0x0018  8     generation
0x0020  8     virtual_size
0x0028  4     logical_sector_size
0x002c  4     physical_sector_size
0x0030  4     cluster_size
0x0034  4     flags
0x0038  8     cluster_count
0x0040  8     metadata_area_offset = 0x00010000
0x0048  8     data_area_offset
0x0050  8     base_descriptor_offset
0x0058  4     base_descriptor_length
0x005c  4     reserved0
0x0060  8     map_root_offset
0x0068  4     map_root_length
0x006c  4     map_entry_size = 16
0x0070  4     incompatible_features
0x0074  4     readonly_compatible_features
0x0078  4     compatible_features
0x007c  4     reserved1
0x0080  16    image_uuid
0x0090  32    image_digest
0x00b0  4     has_image_digest
0x00b4  4     reserved2
0x00b8  3912  reserved
```

`superblock_crc32c` covers all 4096 bytes of the superblock with bytes
`0x0014..0x0017` treated as zero.

`cluster_count` must equal:

```text
ceil(virtual_size / cluster_size)
```

`base_descriptor_offset` is zero when the image has no base image.
`base_descriptor_length` must also be zero when `base_descriptor_offset` is
zero.

`map_root_offset` and `map_root_length` must be non-zero for every valid V1
image.

## Superblock Selection

On open, Scorpi reads both superblocks.

A superblock is valid only if:

- magic matches `SCOIMG\0\0`
- version is supported
- `superblock_size == 4096`
- checksum type is supported
- CRC is valid
- fixed layout constraints are valid
- feature bits are compatible with the requested open mode

Selection rule:

1. If neither superblock is valid, reject the image.
2. If only one superblock is valid, use it.
3. If both are valid, use the one with the larger `generation`.
4. If both are valid and generations are equal, use superblock A.

The generation value is unsigned. V1 does not define wraparound behavior;
writers must not wrap it.

Writers update metadata by writing the inactive superblock with
`generation + 1`, flushing it, and then treating it as the active generation.

## Feature Bits

V1 feature bit assignments:

```text
incompatible_features:
  bit 0  allocation map v1 is required
  bit 1  zero/discard states are required

readonly_compatible_features:
  bit 0  sealed image

compatible_features:
  bit 0  image_digest field may be valid
```

All other bits are reserved.

A V1 writer must set incompatible bit 0. A V1 writer must set incompatible bit
1 if any map entry uses `ZERO` or `DISCARDED`.

If readonly-compatible sealed bit 0 is set, Scorpi must reject writable opens.

## Base Descriptor

The base descriptor records the image below this image in the backing chain.

Offset and length are stored in the selected superblock. The descriptor must
start on a 4096-byte boundary and must fit entirely before `data_area_offset`.

If `base_descriptor_offset == 0`, the image has no base.

V1 descriptor layout:

```text
Offset  Size  Field
0x0000  4     descriptor_size
0x0004  4     checksum_type = 1
0x0008  4     descriptor_crc32c
0x000c  4     flags
0x0010  4     base_uri_length
0x0014  4     reserved0
0x0018  16    base_uuid
0x0028  32    base_digest
0x0048  4     has_base_uuid
0x004c  4     has_base_digest
0x0050  N     base_uri bytes, not NUL-terminated
...           zero padding to descriptor_size
```

`descriptor_size` must be at least `0x0050 + base_uri_length` and must be a
multiple of 4096.

`descriptor_crc32c` covers `descriptor_size` bytes with bytes `0x0008..0x000b`
treated as zero.

V1 supports only `file:` base URIs. Unsupported schemes reject the image until
a future runtime explicitly supports them.

`base_uuid` and `base_digest` are validation fields only. They do not replace
opening and probing the base image. Format, virtual size, and sector sizes are
always discovered from the opened base image.

## Map Root

The map root is an array of fixed-size root entries. It may span multiple
4096-byte pages.

`map_root_offset` must be 4096-byte aligned. `map_root_length` must be a
non-zero multiple of 4096. The map root must fit entirely before
`data_area_offset`.

Each root page is checksummed independently.

Map root page layout:

```text
Offset  Size  Field
0x0000  4     page_size = 4096
0x0004  4     checksum_type = 1
0x0008  4     page_crc32c
0x000c  4     root_entry_count
0x0010  8     first_root_index
0x0018  4072  root entries and zero padding
```

Each root entry is 16 bytes:

```text
Offset  Size  Field
0x0000  8     map_page_offset
0x0008  4     map_page_crc32c
0x000c  4     flags
```

`map_page_offset == 0` means all clusters covered by that map page are
`ABSENT`.

Each root page can contain:

```text
(4096 - 24) / 16 = 254 root entries
```

`root_entry_count` must be less than or equal to 254. A root page covers
`root_entry_count` consecutive root indexes starting at `first_root_index`.

The root page CRC covers all 4096 bytes with bytes `0x0008..0x000b` treated as
zero.

## Map Pages

Each map page is exactly 4096 bytes and is checksummed independently.

Map page layout:

```text
Offset  Size  Field
0x0000  4     page_size = 4096
0x0004  4     checksum_type = 1
0x0008  4     page_crc32c
0x000c  4     map_entry_count
0x0010  8     first_cluster_index
0x0018  4072  map entries and zero padding
```

Each map entry is 16 bytes:

```text
Offset  Size  Field
0x0000  1     state
0x0001  1     flags
0x0002  2     reserved0
0x0004  4     reserved1
0x0008  8     physical_offset
```

Map entry states:

```text
0  ABSENT
1  PRESENT
2  ZERO
3  DISCARDED
```

Each map page can contain:

```text
(4096 - 24) / 16 = 254 map entries
```

`map_entry_count` must be less than or equal to 254. A map page covers
`map_entry_count` consecutive cluster indexes starting at
`first_cluster_index`.

The map page CRC covers all 4096 bytes with bytes `0x0008..0x000b` treated as
zero.

For a cluster index:

```text
entries_per_page = 254
root_index = cluster_index / entries_per_page
entry_index = cluster_index % entries_per_page
```

The map root entry at `root_index` points to the map page containing
`entry_index`. The root entry is found in the root page whose range contains
`root_index`.

The number of map pages required by a virtual disk is:

```text
map_page_count = ceil(cluster_count / 254)
root_page_count = ceil(map_page_count / 254)
```

Rules:

- `ABSENT`, `ZERO`, and `DISCARDED` entries must have `physical_offset == 0`.
- `PRESENT` entries must have `physical_offset >= data_area_offset`.
- `PRESENT` entries must have `physical_offset` aligned to `cluster_size`.
- Entries with cluster indexes greater than or equal to `cluster_count` must be
  zero-filled and treated as invalid if non-zero.

## Cluster Data

Cluster data is addressed by map entry `physical_offset`.

For a `PRESENT` entry, the stored cluster length is:

```text
min(cluster_size, virtual_size - cluster_index * cluster_size)
```

Reads outside the stored cluster length are invalid because such clusters must
only occur at the end of the virtual disk.

Writers must allocate cluster data at `cluster_size` aligned offsets greater
than or equal to `data_area_offset`.

V1 does not require data-cluster checksums.

## Cluster Size Limits

V1 accepts:

```text
64 KiB <= cluster_size <= 4 MiB
cluster_size is a power of two
cluster_size % logical_sector_size == 0
```

The default cluster size is 256 KiB.

`logical_sector_size` must be non-zero and a power of two.
`physical_sector_size` may be zero when unknown. If non-zero, it must be a
power of two and at least `logical_sector_size`.

## Compatibility Summary

A readonly V1 open must reject:

- bad file identifier magic
- mismatched file identifier and superblock UUIDs
- no valid superblock
- unsupported major version
- unsupported minor version not covered by feature flags
- unknown incompatible feature bits
- invalid metadata checksum
- invalid layout alignment or bounds
- invalid base URI
- invalid base identity when identity fields are present
- incompatible chain size or logical sector size

A writable V1 open must additionally reject:

- unknown readonly-compatible feature bits
- sealed images
- any writable base image

## Fixture Generator Requirements

A deterministic fixture generator can create a minimal base `.sco` image with:

```text
file identifier block
superblock A generation 1
empty superblock B
base_descriptor_offset = 0
one map root page
zero map pages, meaning all clusters are ABSENT
data_area_offset = next cluster-size aligned offset after the map root
```

A deterministic overlay fixture can add:

```text
base descriptor with file: URI
map root page
one or more map pages
optional PRESENT clusters at aligned physical offsets
optional ZERO or DISCARDED entries
superblock A/B generation selected by test
```

The generator must calculate CRC-32C after all fields except checksum fields are
written.
