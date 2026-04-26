# Snapshot Image Format Research

## Purpose

This document reviews existing virtual disk and layered-storage formats so the
Scorpi snapshot image design can reuse proven ideas and avoid inherited
complexity.

It is research input for the future Scorpi-native overlay format design. It is
not the format specification.

## Recommendation

Scorpi should support existing formats for compatibility, but should define its
own snapshot overlay format.

Recommended role by format:

- `raw`: simple base image and legacy writable disk format
- `qcow2`: imported base image and readonly backing image
- `vhdx`: possible future import/base image format
- `vmdk`: possible future import/base image format
- Scorpi overlay: native snapshot layer format

The native Scorpi overlay should be small, strict, sparse, checksummed,
feature-flagged, and designed around immutable committed layers plus one
writable active layer.

## Format Survey

### Raw

Raw is a byte-for-byte virtual disk image with no image-level metadata.

Strengths:

- trivial to implement
- fast sequential and random I/O
- easy to inspect and recover
- no format-specific corruption modes
- works well as an immutable base under an overlay

Weaknesses:

- no backing file metadata
- no virtual size metadata beyond host file size
- no sparse allocation map unless relying on host filesystem holes
- no parent identity
- no feature negotiation
- no place for checksums, tombstones, or sealed state

Lessons for Scorpi:

- Keep raw as the simplest base format.
- Do not try to layer snapshot semantics onto raw itself.
- Treat raw fallback carefully because raw has no magic header.

### Qcow2

Qcow2 is QEMU's native copy-on-write image format. It uses fixed-size clusters,
two-level L1/L2 mapping tables, refcount metadata, optional backing files,
zero clusters, bitmaps, compression, encryption, and internal snapshots.

Strengths:

- mature and widely used
- supports sparse allocation
- supports backing chains
- supports internal snapshots
- supports zero clusters and subcluster allocation
- supports dirty bitmaps
- has explicit compatible, incompatible, and autoclear feature bits
- can tune cluster size for space/performance tradeoffs

Weaknesses:

- large implementation surface
- refcount tables make writable support much more complex
- lazy refcounts improve performance but require repair after host crash
- internal snapshots add another snapshot model inside the image
- compression, encryption, external data files, bitmaps, and feature bits all
  increase validation burden
- backing file paths need careful trust and relocation handling
- not ideal as a content-addressed distribution layer because writable metadata
  and refcount mutation are central to the format

Lessons for Scorpi:

- Reuse the L1/L2 idea or a simpler equivalent for scalable lookup.
- Copy feature-bit discipline: unknown incompatible features must fail closed.
- Copy zero-range representation.
- Avoid refcounted shared mutable clusters in the Scorpi overlay v1.
- Avoid internal snapshots inside a single layer file; parent management owns
  the snapshot graph.
- Support qcow2 initially as readonly base/backing input, not as the native
  snapshot layer.

### VHDX

VHDX is Microsoft's modern virtual hard disk format. It supports fixed,
dynamic, and differencing disks. It has a header section with two headers,
region tables, a metadata region, a block allocation table, a log for metadata
updates, payload blocks, sector bitmap blocks for differencing disks, parent
locators, checksums, and explicit sector-size metadata.

Strengths:

- strong crash-safety model for metadata through logging
- duplicated headers with sequence numbers and checksums
- CRC-32C checksums for important structures
- required/optional region model supports extensibility
- explicit virtual size, logical sector size, and physical sector size
- differencing disks use sector bitmaps so partial blocks can fall through to
  the parent at sector granularity
- parent locators include parent identity checks
- UNMAP/discard state is part of the block state model

Weaknesses:

- relatively complex for a first native implementation
- large block sizes, from 1 MB to 256 MB, are less attractive for fine-grained
  snapshot layers unless paired with sector bitmaps
- differencing behavior depends on both BAT state and sector bitmap state
- format is optimized for Hyper-V compatibility, not Scorpi's future
  content-addressed repository model

Lessons for Scorpi:

- Copy the metadata log or a simpler transactional update model.
- Copy duplicated critical headers or an atomic superblock scheme.
- Copy CRC-32C or another explicit checksum for metadata.
- Copy explicit logical and physical sector-size metadata.
- Copy parent identity validation, not just parent location storage.
- Copy the idea that discard/UNMAP must be represented explicitly.
- Avoid very large block granularity unless sub-block presence is also tracked.

### VMDK

VMDK is VMware's virtual disk format family. It includes flat extents, sparse
extents, split files, stream-optimized compressed extents, descriptors, and
snapshot/delta variants.

Strengths:

- broad ecosystem support
- descriptor model can describe one or more extents
- sparse extents use grain directories and grain tables
- stream-optimized variants are useful for appliance distribution
- redundant grain metadata exists in hosted sparse variants
- parent/content identifiers are used to validate chain consistency

Weaknesses:

- a family of related formats rather than one simple format
- descriptor plus extent split creates path and consistency hazards
- snapshot formats differ across VMware products and eras
- compressed stream-optimized layout is good for transport but not ideal for
  direct random-write active disks
- compatibility behavior can be subtle across VMware, VirtualBox, and QEMU

Lessons for Scorpi:

- Avoid a family of many layout variants in v1.
- Avoid requiring sidecar descriptor files for active disks.
- Parent identity is valuable; path alone is not enough.
- Stream-optimized export can be a later repository artifact, not the live
  runtime format.
- Redundant metadata is useful, but it should be paired with clear recovery
  rules.

### VDI

VDI is VirtualBox's native disk format. Public VirtualBox documentation treats
it primarily as a fixed-size or dynamically allocated disk image format.

Strengths:

- simple user model
- dynamic images start small and grow on first writes
- fixed images are predictable and simple
- good enough for local desktop virtualization

Weaknesses:

- less useful as an interchange format than qcow2, VHDX, or VMDK
- not a strong model for distributed snapshot graphs
- not the best source of snapshot-chain or repository lessons

Lessons for Scorpi:

- Keep the user-facing model simple even if the implementation has richer
  internals.
- Dynamic allocation is useful, but first-write growth must be measured and
  optimized.

### Linux dm-thin

Linux device-mapper thin provisioning is not a portable image file format, but
it is highly relevant as a snapshot storage design.

Strengths:

- many thin volumes share one data pool
- recursive snapshots avoid O(depth) lookup by using shared metadata
- external readonly origins are supported
- snapshot descendants do not need activation ordering
- metadata and data can live on separate devices
- supports discard policy controls and no-space behavior

Weaknesses:

- block-device/pool architecture, not a portable single-file format
- requires kernel/device-mapper infrastructure
- parent management must allocate device identifiers and coordinate lifecycle
- not directly usable for Scorpi image push/pull

Lessons for Scorpi:

- Long chains should not mean O(depth) I/O forever.
- Use shared indexing/caches or compaction to control read amplification.
- Separate management identity from runtime mapping.
- No-space behavior and discard behavior must be explicit.

### OCI Images

OCI images are not disk images, but they are the best reference for future
Docker/git-like push and pull.

Strengths:

- content-addressed descriptors
- manifests reference configs and ordered layers
- indexes support multiple variants
- blobs are identified by digest and size
- tags are mutable pointers, digests are identity
- distribution is decoupled from runtime unpacking

Weaknesses:

- filesystem layers are not random-write disk layers
- tar layers are not suitable for VM block I/O
- deletion and garbage collection are registry/implementation concerns

Lessons for Scorpi:

- Use content digests for committed layer identity.
- Keep remote manifests outside the hypervisor runtime.
- Treat tags as names, not identity.
- Design Scorpi overlay layers so they can become digest-addressed blobs.
- Include per-layer and eventually per-chunk verification.

## Cross-Format Lessons

### What To Copy

- Fixed-size allocation units for predictable lookup and allocation.
- Two-level or paged mapping tables for large disks.
- Feature flags with incompatible/compatible classes.
- Explicit virtual size and sector-size metadata.
- Backing path plus parent identity.
- Zero-range representation.
- Discard/tombstone representation.
- Checksummed critical metadata.
- Atomic or logged metadata updates.
- Immutable committed layers.
- Strict validation before accepting guest I/O.
- Clear readonly lower-layer semantics.
- Diagnostic tooling from day one.

### What To Avoid

- Internal snapshot catalogs inside runtime layer files.
- Shared mutable clusters that require refcount updates in multiple places.
- Many layout variants in the first version.
- Required sidecar descriptor files for live disks.
- Silent acceptance of unknown features.
- Trusting backing locations from imported images without policy.
- Compression in active writable layers.
- Encryption in v1 unless it is delegated to a proven lower layer.
- Depending on host filesystem sparseness as the only allocation model.
- Letting discard fall through to parent data.

## Scorpi Overlay Design Direction

The first Scorpi overlay should be a single-file sparse overlay optimized for
correct local I/O and future distribution.

Recommended v1 properties:

- stable magic plus explicit major/minor version fields
- little-endian fields
- feature flags split into incompatible and compatible sets
- virtual disk size
- logical sector size
- physical sector size
- cluster size, likely 64 KiB or 256 KiB
- parent location URI
- parent content identity or generation
- sealed flag
- allocation map
- per-cluster state: absent, present, zero, discarded
- metadata checksum
- optional per-cluster checksum for sealed layers
- atomic metadata update strategy

Recommended v1 constraints:

- no internal snapshots
- no compression for active writable layers
- no encryption in the image format
- no refcounted shared physical clusters
- no multi-file extents
- one writable top layer
- all lower layers readonly
- unknown incompatible features fail closed
- raw fallback only where parent policy allows it

The phrase "fail closed" above means the file open operation should fail rather
than attempt best-effort I/O with unknown semantics.

## Runtime Support Strategy

Scorpi should implement support in this order:

1. Backend abstraction behind `block_if`.
2. Raw backend preserving current behavior.
3. Format probing and explicit raw fallback rules.
4. Chain resolver with cycle/depth/size/sector validation.
5. Scorpi overlay read support.
6. Scorpi overlay write support for one active top layer.
7. Readonly qcow2 support for imported bases.
8. Runtime pause/drain/flush/reopen primitives.

VHDX and VMDK should be treated as future import/base candidates unless a user
requirement makes them urgent.

## Sources

- QEMU qcow2 format specification:
  https://www.qemu.org/docs/master/interop/qcow2.html
- QEMU disk image documentation:
  https://www.qemu.org/docs/master/system/images
- Microsoft VHDX format specification download page:
  https://www.microsoft.com/en-us/download/details.aspx?id=34750
- VMware open-vmdk:
  https://github.com/vmware/open-vmdk
- VMware open-vmdk sparse header:
  https://raw.githubusercontent.com/vmware/open-vmdk/master/vmdk/vmware_vmdk.h
- Oracle VirtualBox disk image documentation:
  https://docs.oracle.com/en/virtualization/virtualbox/6.0/user/vdidetails.html
- Linux device-mapper thin provisioning documentation:
  https://www.kernel.org/doc/html/latest/admin-guide/device-mapper/thin-provisioning.html
- OCI image manifest specification:
  https://github.com/opencontainers/image-spec/blob/main/manifest.md
