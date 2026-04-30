# Scorpi Snapshot Detailed Design

## Purpose

This document turns the snapshot PRD and image-format research into a concrete
design for Scorpi disk snapshot support.

The design has two layers:

- runtime image support inside Scorpi
- snapshot and repository management outside Scorpi

The detailed task breakdown should be derived from this document after the
design is reviewed.

## Decision Summary

Scorpi will support snapshot-capable disks by opening a single configured top
image path and resolving the local backing chain automatically.

Scorpi will define a native sparse overlay format for snapshot layers. The
native overlay is optimized for Scorpi's runtime and future parent-managed
repository workflows.

Existing formats remain important:

- `raw` is supported as a simple base image and legacy writable disk.
- `qcow2` is supported first as a readonly base or lower backing image.
- `vhdx` and `vmdk` are future import/base candidates.
- native Scorpi overlay is the writable and committed snapshot layer format.

Scorpi runtime will not manage snapshot graphs, retention, compaction, push,
pull, refs, tags, or garbage collection. Those belong to the parent management
system.

## Current Codebase Fit

The current block-device path already has a useful abstraction point.

Virtio block and AHCI open disks through `blockif_open()` and then submit
requests through `blockif_read()`, `blockif_write()`, `blockif_flush()`, and
`blockif_delete()`.

The design keeps device emulations snapshot-agnostic:

```text
virtio-blk / AHCI
        |
      block_if
        |
  image chain backend
        |
 raw / qcow2 / Scorpi overlay layers
```

The block interface should remain the compatibility boundary for emulated
devices. Snapshot-aware behavior is introduced behind `block_if`.

## Architecture

### Components

Runtime components:

- block interface adapter
- image backend registry
- raw image backend
- qcow2 readonly backend
- Scorpi overlay backend
- chain resolver
- chain I/O engine
- block drain/flush/reopen control path
- diagnostics and validation reporting

Parent management components:

- image inventory
- snapshot graph metadata
- active image selection
- commit/snapshot operation
- compaction
- retention and garbage collection
- push/pull and remote manifests
- lock coordination with running Scorpi processes

### Runtime Ownership

Scorpi owns:

- local file format parsing needed for I/O
- local backing-chain resolution
- runtime validation
- serving reads and writes
- live disk drain/flush/reopen primitives

Scorpi does not own:

- global image identity
- snapshot names
- branches
- tags
- remote repositories
- graph compaction
- graph garbage collection
- retention policy
- long-term metadata database

## VM Configuration

The VM config continues to point at one disk path:

```yaml
devices:
  pci:
    - device: virtio-blk
      slot: 2
      path: /var/lib/scorpi/images/vm1-active.sco
```

`path` is interpreted as the top image.

Scorpi opens the top image, probes its format, reads its backing metadata, and
resolves the chain:

```text
vm1-active.sco
  -> snap-003.sco
    -> snap-002.sco
      -> base.qcow2
```

The config does not list the chain. Snapshot operations should not require VM
metadata rewrites.

### Optional Future Disk Properties

The following properties may be added later:

```yaml
- device: virtio-blk
  path: /var/lib/scorpi/images/vm1-active.sco
  image.max_chain_depth: 32
  image.raw_fallback: false
  image.expected_format: scorpi-overlay
```

These are validation controls only. They do not make Scorpi a snapshot manager.

## Runtime Image API

The internal image layer should be separate from the existing block queue.
`block_if` should dispatch completed requests into an image-chain object.

Suggested internal types:

```c
enum scorpi_image_format {
	SCORPI_IMAGE_RAW,
	SCORPI_IMAGE_QCOW2,
	SCORPI_IMAGE_OVERLAY,
};

enum scorpi_extent_state {
	SCORPI_EXTENT_ABSENT,
	SCORPI_EXTENT_PRESENT,
	SCORPI_EXTENT_ZERO,
	SCORPI_EXTENT_DISCARDED,
};

struct scorpi_image_info {
	enum scorpi_image_format format;
	uint64_t virtual_size;
	uint32_t logical_sector_size;
	uint32_t physical_sector_size;
	uint32_t cluster_size;
	bool readonly;
	bool sealed;
	bool has_backing;
	char *backing_uri;
	uint8_t parent_digest[32];
	bool has_parent_digest;
};

struct scorpi_image_extent {
	uint64_t offset;
	uint64_t length;
	enum scorpi_extent_state state;
};

struct scorpi_image_ops {
	const char *name;
	int (*probe)(int fd, uint32_t *score);
	int (*open)(const char *path, int fd, bool readonly, void **state);
	int (*get_info)(void *state, struct scorpi_image_info *info);
	int (*map)(void *state, uint64_t offset, uint64_t length,
	    struct scorpi_image_extent *extent);
	int (*read)(void *state, void *buf, uint64_t offset, size_t len);
	int (*write)(void *state, const void *buf, uint64_t offset, size_t len);
	int (*discard)(void *state, uint64_t offset, uint64_t len);
	int (*flush)(void *state);
	int (*close)(void *state);
};
```

The exact C names can change during implementation. The important part is that
format drivers can expose extent state separately from reading bytes.

### Why `map()` Exists

Layered reads need to know whether a range is present, absent, zero, or
discarded.

If the top layer reports `PRESENT`, Scorpi reads from it.

If it reports `ZERO` or `DISCARDED`, Scorpi returns zeroes and does not inspect
parents.

If it reports `ABSENT`, Scorpi continues to the next layer.

This avoids treating discard as a hole that falls through to stale parent data.

## Chain Resolver

### Open Algorithm

When a block device opens:

1. Read `path` from device config.
2. Open the path.
3. Probe registered image backends.
4. Open the best matching backend.
5. Read image metadata.
6. If there is a parent location URI, resolve it according to its URI scheme.
7. Repeat until an image has no parent location URI.
8. Validate the full chain.
9. Build an in-memory `scorpi_image_chain`.
10. Start block worker threads using the chain object.

### Validation Rules

The resolver must reject:

- cycles
- missing backing resources
- unsupported formats
- unsupported format features
- chain depth above configured maximum
- incompatible virtual sizes
- incompatible logical sector sizes
- incompatible writable layering
- multiple writable layers
- writable lower layers
- top layer readonly when the VM requests write access
- `file:` parent URIs that escape allowed roots, when policy is configured
- imported qcow2 backing locations that violate parent policy

Default runtime maximum chain depth should be conservative. A suggested first
default is 32 layers. The parent management system should compact earlier.

### Parent Location Resolution

Backing relationships should be stored as parent location URIs, not plain
paths. This keeps the image metadata extensible for future network-backed image
stores.

Current runtime support is limited to the `file:` scheme. Future schemes such as
`https:`, `s3:`, `scorpi:`, or repository-specific schemes may be added later,
but Scorpi should reject unsupported schemes rather than attempting best-effort
I/O.

Supported `file:` URI forms:

- `file:///absolute/path/to/parent.sco`
- `file:relative/path/to/parent.sco`

Relative `file:` URI paths are resolved relative to the directory containing
the child image.

Example:

```text
/images/vm1/active.sco
  parent_uri: file:snap-003.sco

resolved parent:
  /images/vm1/snap-003.sco
```

Scorpi should store both the original parent URI and the resolved local path in
diagnostics. It should not rewrite image metadata.

Absolute `file:` URIs are subject to parent policy. A future parent management
system may allow image stores to reference parents outside the child directory,
but the resolver must make that behavior explicit and auditable.

### Cycle Detection

Cycle detection should use canonical file identity where available:

- device id and inode on POSIX filesystems
- canonical absolute path as a fallback
- optional image content identity when present

The resolver should reject a chain if the same file identity appears twice.

## Chain I/O Engine

### Read Path

Reads are split on extent boundaries.

For each range:

1. Check resolved extent cache.
2. If cache miss, query layers from top to base with `map()`.
3. If a layer returns `PRESENT`, read from that layer.
4. If a layer returns `ZERO` or `DISCARDED`, zero-fill the range.
5. If all layers return `ABSENT`, zero-fill the range.
6. Cache the resolved owner/state for future reads.

Pseudo-code:

```text
read(offset, len):
  while len > 0:
    entry = cache.lookup(offset)
    if entry missing:
      entry = resolve_extent(offset, len)
      cache.insert(entry)

    n = min(len, entry.length)
    if entry.state == PRESENT:
      entry.layer.read(buf, offset, n)
    else:
      memset(buf, 0, n)
```

### Write Path

Writes always go to the top layer.

The top image must be writable and must support writes. Lower layers are never
modified by Scorpi runtime.

After a successful write, the extent cache entries overlapping the written
range are invalidated or updated to point at the top layer.

### Discard Path

Discard goes to the top layer only.

The top layer records `DISCARDED` or `ZERO` state for the range. Reads after
discard must return zeroes or the format-defined discard result. They must not
fall through to parent data.

If the top backend cannot persist discard state, `blockif_candelete()` should
not advertise discard support for that disk.

### Flush Path

Flush calls the top layer's `flush()` implementation.

For the Scorpi overlay backend, flush must persist:

- written cluster data required by completed guest writes
- allocation map updates required to find those clusters
- discard/zero state updates
- active metadata generation

Flush does not need to flush readonly lower layers.

### Cache

The chain engine should use an extent-resolution cache:

```text
logical range -> layer index + state + generation
```

Initial implementation can use a simple interval tree or page-sized hash keyed
by cluster index. A cluster-index cache is simpler and fits fixed cluster
overlays.

Cache invalidation:

- write: invalidate affected clusters
- discard: invalidate or replace affected clusters with top-layer zero/discard
- reopen: drop entire cache
- lower-layer mutation: unsupported while open

## Scorpi Overlay Format

### Format Goals

The native overlay format should be:

- sparse
- simple to validate
- safe after host crashes
- efficient for random reads and writes
- explicit about zero and discard state
- strict about feature compatibility
- suitable for immutable committed layers
- compatible with future content-addressed push/pull

### Format Name

Working name:

```text
Scorpi Overlay
```

Suggested extension:

```text
.sco
```

Suggested magic:

```text
SCOIMG\0\0
```

The extension is advisory only. Format probing uses magic. The magic does not
encode the format version; versioning is stored in explicit superblock fields.

Current version:

```text
format_major = 2
format_minor = 0
```

The current on-disk layout is specified in
[sco_v1_format.md](sco_v1_format.md). That document is the parser contract.
The old root/map V1 layout is intentionally not supported.

### File Layout

Current layout:

```text
0x00000000  File identifier block
0x00001000  Superblock A
0x00002000  Superblock B
0x00003000  Reserved
0x00010000  Metadata area
            - parent descriptor
            - fixed two-slot allocation table
cluster data area
```

All multi-byte fields are little-endian.

The file identifier block is never overwritten after creation. It allows the
file to be identified even if a later metadata update is interrupted.

The active superblock is selected by generation number and checksum. Updates
write the inactive superblock, flush it, and then make it the newest valid
generation.

### Superblock

Suggested fields:

```c
struct sco_superblock {
	uint8_t magic[8];              /* "SCOIMG\0\0" */
	uint16_t format_major;
	uint16_t format_minor;
	uint32_t header_size;
	uint32_t checksum_type;
	uint32_t header_crc32c;
	uint32_t flags;
	uint64_t generation;
	uint64_t virtual_size;
	uint32_t logical_sector_size;
	uint32_t physical_sector_size;
	uint32_t cluster_size;
	uint64_t cluster_count;
	uint64_t data_area_offset;
	uint64_t parent_descriptor_offset;
	uint64_t parent_descriptor_length;
	uint64_t table_offset;
	uint32_t single_generation_table_length;
	uint32_t table_slot_size;
	uint8_t image_uuid[16];
	uint8_t parent_uuid[16];
	uint8_t parent_digest[32];
	uint32_t incompatible_features;
	uint32_t compatible_features;
	uint32_t readonly_compatible_features;
	uint8_t reserved[...];
};
```

The exact packed layout is specified in [sco_v1_format.md](sco_v1_format.md).

### Version Compatibility

Scorpi Overlay uses both explicit version fields and feature flags.

Version rules:

- unsupported `format_major` values must fail closed
- supported `format_major` with equal or lower `format_minor` may open if all
  required feature flags are supported
- supported `format_major` with higher `format_minor` may open only if unknown
  behavior is covered by compatible or readonly-compatible feature flags
- version fields describe the base structure layout
- feature flags describe optional behavior within that layout

Scorpi supports:

```text
format_major = 2
format_minor = 0
```

### Feature Flags

Feature flags are split into classes:

- incompatible: unknown bits mean open must fail
- readonly-compatible: unknown bits allow readonly open but reject writable open
- compatible: unknown bits may be ignored

Initial incompatible features:

- fixed two-slot allocation table
- sealed layer enforcement

Potential future features:

- per-cluster checksums
- chunk-addressed external storage
- compression for sealed layers
- encryption metadata
- larger map fanout

Compression and encryption are not enabled in the current format.

### Parent Descriptor

The parent descriptor records the minimum information needed to locate and
identify the parent:

```text
parent_uri = "file:snap-003.sco"
parent_uuid = ...
parent_digest = sha256:...
```

Parent URI is used for chain resolution. Scorpi resolves only `file:` URIs.
Unsupported schemes fail closed. The parent management system may materialize
network or repository-backed images into local `file:` images before launch or
before a live reopen.

Parent UUID or digest is used to detect accidental parent mismatch.

Parent format, virtual size, and sector size are not stored as authoritative
parent descriptor fields. Scorpi discovers them by opening the parent image and
reading its metadata. The resolver then validates that the opened parent is
compatible with the child.

An optional parent format hint can be added later if probing becomes expensive
or ambiguous, but it must remain only a hint. It must never override the
format, size, or sector metadata discovered from the actual parent image.

If parent identity is present and does not match the opened parent, Scorpi must
reject the chain.

### Allocation Unit

Default cluster size should be chosen during implementation benchmarking.

Recommended initial default:

```text
256 KiB
```

Rationale:

- smaller metadata than 64 KiB for large disks
- better random-write efficiency than 1 MiB or larger blocks
- simple alignment and caching
- still reasonable for content-addressed storage later

Allowed range:

```text
64 KiB <= cluster_size <= 4 MiB
cluster_size must be a power of two
cluster_size must be a multiple of logical_sector_size
```

### Cluster States

Each virtual cluster has a state:

```text
ABSENT     no local data; read from parent
PRESENT    data stored in this image
ZERO       reads return zeroes; do not read parent
DISCARDED  reads return zeroes; do not read parent
```

`ZERO` and `DISCARDED` may behave the same for read I/O. They are kept
distinct so tooling can distinguish guest zero writes from guest discard intent.

### Allocation Map

The allocation map is a fixed flat table allocated at image creation.

Each virtual cluster has two 16-byte slots:

```text
cluster N
  slot A
  slot B
```

Each slot records:

```text
generation
physical_cluster
crc32c
state
flags
```

The valid slot with the highest generation is active. The CRC includes the
image UUID, virtual cluster index, slot index, and slot contents with the CRC
field zeroed. All-zero slots mean implicit `ABSENT`.

The single-generation table size is:

```text
align_up(cluster_count * 16, 4096)
```

`scorpi-image create` keeps this size at or below 32 MiB by increasing cluster
size when needed. The on-disk table stores two slots per cluster, so on-disk
metadata for the table is at most 64 MiB. The runtime loads the whole resolved
table into memory during open.

### Write Allocation

When writing to an absent cluster:

1. Allocate a new physical cluster in the file.
2. For partial-cluster writes, materialize missing bytes by reading from the
   chain or zero-filling as appropriate.
3. Write the complete or partial cluster data.
4. Flush data if required by current durability mode.
5. Write the inactive table slot with `generation + 1`.
6. Flush the table slot and update the in-memory resolved table.

For full-cluster writes, parent materialization is not required.

### Partial Writes

Partial-cluster writes are the main correctness trap.

If a guest writes only part of an absent cluster, the unwritten parts must still
read as the previous chain-visible contents.

There are two possible designs:

1. materialize whole cluster on first partial write
2. support subcluster allocation bitmap

The current format uses whole-cluster materialization. It is simpler and safer.

Subcluster allocation can be added later if write amplification is too high.

### Discard And Zero Handling

Guest discard over a full cluster:

- mark cluster `DISCARDED`
- free local physical storage if possible
- do not read parent for that cluster

Guest discard over a partial cluster:

- the runtime may materialize the cluster and zero the discarded range
- or record a future subcluster tombstone feature

The runtime keeps partial discard simple:

- if the cluster is present, zero the discarded sector range in the local data
- if the cluster is absent, materialize the cluster from the chain, zero the
  discarded sector range, and mark it present

This avoids subcluster tombstone complexity in v1.

### Sealed Layers

A sealed layer is immutable.

Scorpi runtime may open sealed layers readonly.

Scorpi runtime must reject write access to a sealed layer.

The parent management system seals layers during commit/snapshot operations.
Sealed layers are suitable for digest calculation and push/pull.

### Checksums

The current format requires checksums for critical metadata:

- superblocks
- metadata descriptors
- table slots

CRC-32C is a reasonable metadata checksum because it is widely used and
hardware-accelerated on many platforms.

Per-cluster data checksums should be optional and required only for
sealed layers if enabled. The parent repository layer may calculate stronger
content digests independently.

### Crash Consistency

The overlay must recover to either the old valid metadata generation or the new
valid metadata generation after host crash.

Table slot updates provide the metadata publication boundary:

1. Write new or changed data clusters.
2. Flush data before publishing metadata.
3. Write the inactive slot with incremented generation and CRC.
4. Flush the slot.
5. On next open, select the highest-generation valid slot for each cluster.

This avoids both a general-purpose metadata journal and runtime metadata
allocation.

Unreferenced data written before a crash may remain as leaked space inside the
file. Offline repair can reclaim it later.

### Free Space Reuse

The runtime maintains an in-memory data-cluster free bitmap.

The bitmap is rebuilt on open by scanning the resolved fixed table and marking
every active `PRESENT` physical cluster as used. It is not persisted and is not
authoritative; the fixed table is the source of truth.

Allocation first reuses a free physical cluster, then appends to the file only
when no reusable cluster exists. Reuse is allowed because table entries contain
the physical cluster number instead of deriving physical placement from the
virtual cluster index.

When an update makes an old `PRESENT` physical cluster unreachable, the runtime
releases that physical cluster in memory only after the replacement table slot
has been written and fsynced. This keeps crash recovery simple:

- before the slot commit, the old entry remains active
- after the slot commit, reopen derives the same free-space state by scanning
  the table

If allocation writes data but fails before the metadata slot is committed, the
new physical cluster may remain leaked until reopen. That is acceptable because
it avoids reusing a cluster whose publication status is ambiguous.

The parent system should still rotate active layers and compact offline for
long-lived images with significant churn.

## Qcow2 Support Scope

Initial qcow2 support should be readonly.

Accepted qcow2 subset:

- valid qcow2 magic and supported version
- supported cluster size
- L1/L2 tables
- normal allocated clusters
- zero clusters
- backing file metadata
- compatible feature bits that are understood or safe to ignore

Rejected initially:

- encryption
- compression
- internal snapshots as a runtime-visible snapshot model
- external data files
- unknown incompatible features
- dirty bitmap features unless explicitly ignored safely
- writable open

The parent management system can use external tools to convert unsupported
qcow2 images into raw or Scorpi overlay-compatible bases.

## Raw Support Scope

Raw support should preserve current behavior for simple disks.

Raw images:

- have no parent location URI
- use file size as virtual size
- are always fully present
- may be writable for legacy non-snapshot disks
- may be readonly lower layers under a Scorpi overlay

Raw fallback should be configurable by parent policy because raw has no magic.

## Live Snapshot Flow

Scorpi does not create snapshots itself, but the parent needs runtime support
for live operations.

### Stopped VM Commit

For a stopped VM:

1. Parent seals current active overlay.
2. Parent creates a new writable overlay whose parent is the sealed layer.
3. Parent preserves the configured top image path or updates parent-owned VM
   inventory.
4. Scorpi sees only the new top image at next start.

### Running VM Commit

For a running VM:

1. Parent asks Scorpi to pause or block disk I/O.
2. Scorpi drains outstanding block requests.
3. Scorpi flushes the active image.
4. Parent seals the current active overlay.
5. Parent creates a new writable overlay above it.
6. Parent makes the configured top image path resolve to the new overlay.
7. Parent asks Scorpi to reopen the disk.
8. Scorpi resolves and validates the new chain.
9. Scorpi swaps the chain object.
10. Scorpi resumes disk I/O.

If reopen fails, Scorpi should keep the old chain paused and report the error.
The parent can either repair the new top image or ask Scorpi to resume the old
chain if it remains valid and writable.

### Stable Top Path Options

The parent can preserve a stable top path in several ways:

- make the top path a symlink to the active overlay
- use a small pointer file outside Scorpi image format
- replace the top overlay file atomically
- update parent-owned VM inventory

Scorpi only requires that the configured path opens to the desired top image
when `reopen` is requested.

The detailed parent design should choose one policy. Symlink replacement is
simple but requires careful diagnostics and locking.

## Locking And Mutation Rules

Runtime rules:

- Scorpi takes a writer lock on the top writable image.
- Scorpi takes reader locks on lower layers.
- A lower layer in an open chain must not be modified.
- Parent compaction must create new files and swap references; it must not
  rewrite files Scorpi has open.
- Parent deletion must wait until no Scorpi process has the file open.

Lock implementation options:

- advisory `fcntl` locks
- sidecar lock files
- parent daemon lease table
- combination of OS locks and daemon ownership

Scorpi should use OS advisory locks where available and expose diagnostics so the
parent can reconcile open images after crashes.

## Parent Management Design Contract

The parent image store should model a snapshot graph:

```text
image node:
  id
  local path
  format
  parent id
  digest
  sealed
  refs
  created_at
  virtual_size
  logical_sector_size
  unique_bytes
  shared_bytes
  overwritten_bytes
```

Scorpi runtime does not read this graph. It reads only local image files.

The parent graph must be able to answer:

- which top image belongs to a VM
- which image nodes are pinned
- which layers are safe to compact
- which layers are safe to delete
- which layers are already pushed
- which layers need pull/materialization

## Storage Efficiency Design

Storage efficiency is primarily a parent responsibility, but the overlay format
must provide the necessary primitives.

### Runtime Measures

Scorpi runtime provides:

- sparse overlays
- zero/discard states
- extent map diagnostics
- chain depth reporting
- optional per-layer allocated-byte reporting

### Parent Measures

The parent should track:

- chain depth
- active layer size
- unique bytes
- shared bytes
- overwritten bytes
- discard-hidden bytes
- read amplification
- refs and pins

### Compaction

Compaction creates a new layer or chain. It does not mutate published or open
layers in place.

Linear compaction:

```text
base -> a -> b -> c -> active
```

may become:

```text
base -> compacted-abc -> active
```

Branch-aware compaction preserves shared ancestors and compacts private tails
where possible.

The native overlay format helps compaction by exposing allocation states and
zero/discard markers clearly.

## Diagnostics

Scorpi should expose a resolved chain report:

```json
{
  "path": "/images/vm1-active.sco",
  "virtual_size": 68719476736,
  "logical_sector_size": 512,
  "chain_depth": 3,
  "layers": [
    {
      "index": 0,
      "path": "/images/vm1-active.sco",
      "format": "scorpi-overlay",
      "readonly": false,
      "sealed": false,
      "cluster_size": 262144
    },
    {
      "index": 1,
      "path": "/images/snap-003.sco",
      "format": "scorpi-overlay",
      "readonly": true,
      "sealed": true,
      "cluster_size": 262144
    },
    {
      "index": 2,
      "path": "/images/base.qcow2",
      "format": "qcow2",
      "readonly": true
    }
  ]
}
```

Diagnostics should be available through the future control/library boundary.

Initial CLI support can be a local debug command or log output during disk
open.

## Error Handling

Errors should distinguish:

- open failure
- unsupported format
- unsupported feature
- invalid chain
- parent mismatch
- cycle detected
- chain too deep
- incompatible virtual size
- incompatible sector size
- top image readonly
- lower layer writable
- metadata checksum failure
- no space
- flush failure
- reopen failure

Library-facing errors should not call `err()` or terminate the process.

The existing executable may still map these errors to process exit codes.

## Testing Strategy

### Unit Tests

Add tests for:

- format probing
- raw open
- overlay header parse
- overlay invalid checksum rejection
- superblock generation selection
- chain path resolution
- cycle detection
- size mismatch rejection
- sector mismatch rejection
- unknown incompatible feature rejection
- parent identity mismatch rejection

### I/O Tests

Add tests for:

- read from base
- read from overlay
- read through multiple layers
- absent range returns parent data
- absent range with no parent returns zero
- full-cluster write
- partial-cluster write materializes parent data
- discard prevents parent fallthrough
- flush persists table updates
- reopen sees previous writes

### Crash Simulation Tests

Add tests that interrupt overlay updates at controlled points:

- data written, metadata not committed
- inactive table slot partially written
- inactive superblock partially written
- both superblocks valid with different generations
- newest superblock checksum invalid

Expected behavior: open selects the newest valid committed generation.

### Integration Tests

Add tests through `block_if`:

- virtio-blk can open raw as before
- virtio-blk can open a Scorpi overlay backed by raw
- AHCI can open a Scorpi overlay backed by raw
- readonly lower layers are not written
- disk reopen swaps to a new chain after drain

### Compatibility Tests

Add qcow2 fixtures:

- simple readonly qcow2
- qcow2 with backing file
- qcow2 with zero clusters
- qcow2 with unsupported features that must be rejected

Fixtures should be small and generated reproducibly.

## Migration Plan

### Phase 1: Internal Image Backend Abstraction

- Add image backend interfaces.
- Move current raw-file I/O behind a raw backend.
- Keep `blockif_open()` public behavior unchanged.
- Preserve existing tests and VM launch behavior.

### Phase 2: Chain Resolver

- Add format probing.
- Add chain object.
- Add validation rules.
- Add diagnostics.
- Raw files resolve as single-layer chains.

### Phase 3: Scorpi Overlay Readonly

- Define v1 file structures.
- Implement parser and validator.
- Implement map lookup.
- Implement readonly read-through chains.

### Phase 4: Scorpi Overlay Writable Top

- Implement top-layer writes.
- Implement whole-cluster materialization.
- Implement discard/zero states.
- Implement flush and crash-safe metadata commit.

### Phase 5: Qcow2 Readonly

- Implement minimal qcow2 parser.
- Support L1/L2 mapping and zero clusters.
- Support qcow2 backing metadata.
- Reject unsupported features.

### Phase 6: Runtime Reopen

- Add block pause/drain/flush/reopen path.
- Add control/library API for parent-managed live commits.
- Add integration tests.

### Phase 7: Parent Management

- Build image inventory and snapshot graph outside Scorpi runtime.
- Implement commit/snapshot commands.
- Implement storage accounting.
- Implement compaction and GC.

### Phase 8: Repository Workflows

- Add content-addressed layer digests.
- Add remote manifests.
- Add push and pull.
- Add verification and materialization.

## Open Design Questions

- Should `.sco` v1 support subcluster bitmaps, or should that wait for
  v2?
- Should default cluster size be 64 KiB or 256 KiB after benchmarking?
- Should raw fallback default to enabled for local CLI and disabled for the
  parent daemon?
- Should parent identity require UUID, digest, or both?
- Should Scorpi require advisory locks in v1 or make them configurable?
- Should the stable top path be a symlink, pointer file, or parent inventory
  entry?
- Should per-cluster checksums be required for sealed layers in v1?
- Should qcow2 support be implemented directly or initially delegated through a
  conversion/import tool?
- What is the exact control API shape for disk drain/reopen?

## Design Risks

- Whole-cluster materialization may increase write amplification for small
  random writes.
- Append-only metadata and data allocation may waste space in long-lived active
  layers.
- Readonly qcow2 support can still be large if the accepted feature subset is
  not kept strict.
- Live reopen is sensitive to races between parent file swaps and runtime
  chain validation.
- Raw fallback can misidentify corrupt or wrong files as raw disks.
- Advisory locks are not a complete distributed coordination mechanism.

## Final Position

Scorpi should own a narrow, purpose-built overlay format, not the full snapshot
management system.

The runtime image stack should be strict and small:

```text
VM config path
  -> format probe
  -> local chain resolver
  -> validated chain
  -> raw/qcow2/Scorpi-overlay I/O
```

The parent management system should own the product behavior:

```text
commit / branch / tag / compact / gc / push / pull
  -> materialized local top image
  -> Scorpi runtime opens that image
```

This split keeps Scorpi compatible with existing image ecosystems while giving
the project control over correctness, storage efficiency, and future repository
workflows.
