# Scorpi `.sco` Image Format

This document specifies the current `.sco` on-disk layout. It replaces the old
V1 root/map-page design. Scorpi does not keep compatibility with that older
layout.

The design goal is simple, bounded, crash-safe metadata:

- no runtime metadata allocation
- no metadata fragmentation
- no SCO-internal metadata exhaustion
- O(1) cluster lookup
- full metadata table resident in memory after open

## Constants

```text
SCO_MAGIC                         = "SCOIMG\0\0"
SCO_FORMAT_MAJOR                  = 2
SCO_FILE_ID_OFFSET                = 0x00000000
SCO_FILE_ID_SIZE                  = 0x00001000
SCO_SUPERBLOCK_A_OFFSET           = 0x00001000
SCO_SUPERBLOCK_B_OFFSET           = 0x00002000
SCO_SUPERBLOCK_SIZE               = 0x00001000
SCO_METADATA_AREA_OFFSET          = 0x00010000
SCO_METADATA_PAGE_SIZE            = 0x00001000
SCO_DEFAULT_CLUSTER_SIZE          = 0x00040000
SCO_MIN_CLUSTER_SIZE              = 0x00010000
SCO_MAX_CLUSTER_SIZE              = 0x00400000
SCO_TABLE_SLOT_SIZE               = 16
SCO_TABLE_SLOTS_PER_ENTRY         = 2
SCO_MAX_SINGLE_TABLE_BYTES        = 32 MiB
```

## File Layout

```text
0x00000000  file identifier block
0x00001000  superblock A
0x00002000  superblock B
0x00003000  reserved header area
0x00010000  optional base descriptor
table_offset fixed allocation table
data_area_offset data clusters
```

The fixed allocation table is allocated at image creation. It never grows and
never allocates additional metadata pages at runtime.

`scorpi-image create` chooses the smallest cluster size starting at 256 KiB that
keeps the single-generation table at or below 32 MiB. The on-disk table stores
two slots per cluster, so the table consumes at most 64 MiB on disk.

For each virtual disk cluster:

```text
table entry = slot A + slot B
slot size   = 16 bytes
entry size  = 32 bytes
```

The runtime loads the whole resolved table into memory during open. Images whose
single-generation table would exceed 32 MiB are not created at the current
cluster size; creation increases cluster size until the limit is satisfied or
fails at the maximum cluster size.

## Superblock

Superblocks are still duplicated. The valid superblock with the highest
generation is selected.

```text
Offset  Size  Field
0x0000  8     magic = "SCOIMG\0\0"
0x0008  2     format_major = 2
0x000a  2     format_minor = 0
0x000c  4     superblock_size = 4096
0x0010  4     checksum_type = CRC32C
0x0014  4     superblock_crc32c
0x0018  8     generation
0x0020  8     virtual_size
0x0028  4     logical_sector_size
0x002c  4     physical_sector_size
0x0030  4     cluster_size
0x0038  8     cluster_count
0x0040  8     metadata_area_offset = 0x10000
0x0048  8     data_area_offset
0x0050  8     base_descriptor_offset
0x0058  4     base_descriptor_length
0x005c  4     reserved
0x0060  8     table_offset
0x0068  4     single_generation_table_length
0x006c  4     table_slot_size = 16
0x0070  4     incompatible_features
0x0074  4     readonly_compatible_features
0x0078  4     compatible_features
0x007c  4     reserved
0x0080  16    image_uuid
0x0090  32    image_digest
0x00b0  4     has_image_digest
0x00b4  4     reserved
0x00b8  3912  reserved
```

`single_generation_table_length` is the 4096-byte aligned size of one slot array:

```text
align_up(cluster_count * 16, 4096)
```

The total on-disk allocation table length is:

```text
single_generation_table_length * 2
```

## Table Slot

Each cluster has two slots. The valid slot with the highest generation is the
active slot. If an update is interrupted while writing the inactive slot, the
old slot remains valid.

```text
Offset  Size  Field
0x0000  4     generation
0x0004  4     physical_cluster
0x0008  4     crc32c
0x000c  1     state
0x000d  1     flags
0x000e  2     reserved
```

States:

```text
0 ABSENT
1 PRESENT
2 ZERO
3 DISCARDED
```

For `PRESENT`, `physical_cluster` is:

```text
physical_offset / cluster_size
```

For all other states, `physical_cluster` must be zero.

The slot CRC includes:

- image UUID
- virtual cluster index
- slot index
- the slot bytes with `crc32c` treated as zero

This prevents a valid slot from being copied to another cluster or slot.

All-zero slots are treated as implicit `ABSENT`.

## Write Ordering

Writes that do not change metadata, such as overwriting an already-present
cluster with non-zero data, write data in place.

Writes that change metadata use this order:

```text
1. write new data cluster, if the new state is PRESENT
2. fsync
3. write inactive table slot with generation + 1 and CRC
4. fsync
5. update the in-memory resolved table
```

Crash outcomes:

```text
crash before slot write  -> old slot remains active
crash during slot write  -> new slot CRC is invalid, old slot remains active
crash after slot write   -> new slot is active
```

Full-cluster zero writes and full-cluster discards publish `ZERO` or
`DISCARDED` slots. Host hole punching is best-effort after the metadata slot is
committed.

## Data Cluster Allocation

Data clusters are allocated from the physical data area. The on-disk table is
authoritative; the free-space state is not stored separately on disk.

On open, the runtime scans the resolved table and builds an in-memory bitmap of
physical data clusters referenced by active `PRESENT` entries. Allocation first
reuses a free physical cluster from this bitmap, then appends to the file only
when no reusable cluster exists.

When a metadata commit replaces a `PRESENT` entry with `ZERO`, `DISCARDED`, or
another `PRESENT` physical cluster, the old physical cluster is released only
after the new table slot has been written and fsynced. This makes the table slot
publication the durability boundary. If a crash happens before publication, the
old entry remains active. If a crash happens after publication, reopening the
image rebuilds the free bitmap from the committed table and can reuse the old
cluster.

The in-memory free bitmap is disposable. A failed write or interrupted process
may temporarily leak an unreferenced physical cluster, but reopen rebuilds the
bitmap from table state.

## Metadata Exhaustion

This format has no runtime metadata allocator. Therefore the old classes of
metadata exhaustion and metadata fragmentation are removed.

The only creation-time limit is the 32 MiB single-generation table cap. If the
requested virtual size would exceed that cap, creation increases cluster size.
If the table is still too large at the maximum cluster size, creation fails
instead of creating an image that can exhaust metadata later.

## Compatibility

The runtime supports only this fixed-table layout. Older root/map `.sco` images
are rejected.
