# Scorpi Snapshot Product Requirements

## Purpose

This document defines the product requirements for disk snapshot support in
Scorpi and the future parent management system.

It is intentionally not a detailed technical design. It should be used as the
input for a later design document and then split into implementation tasks.

## Summary

Scorpi should support snapshot-capable disks without making the hypervisor
responsible for snapshot management.

The VM configuration should identify only the top local image file. Scorpi
should automatically detect the image format, resolve the local backing chain
from image metadata, and serve guest I/O through that chain.

The parent management system is responsible for all higher-level snapshot and
image lifecycle behavior, including commit-like snapshots, restore points,
branching, push/pull, compaction, retention, garbage collection, and repository
metadata.

## Terminology

- Scorpi runtime: the hypervisor executable/library path that opens disks and
  serves VM I/O.
- Parent management system: the future daemon or product layer that manages VM
  inventory, image inventory, snapshots, remotes, policies, and user commands.
- Top image: the image path referenced by the VM configuration. This is the
  only image path Scorpi needs as input.
- Backing chain: the linear chain of images reachable from the top image by
  following backing metadata.
- Snapshot graph: the full parent-managed tree or DAG of image states,
  branches, commits, tags, and active VM roots.
- Active image: the writable top image used by a running or stopped VM.
- Committed layer: an immutable image layer produced by a user-visible
  snapshot or commit operation.
- Overlay: a sparse image that stores changed blocks and points at a backing
  image.

## Goals

- Allow VM configs to continue referencing a single disk path.
- Automatically detect supported image formats by file content, not file
  extension.
- Automatically resolve the local backing chain from the configured top image.
- Support `raw` and `qcow2` as base image formats.
- Support snapshot overlays above `raw`, `qcow2`, and future image formats.
- Keep Scorpi runtime independent from snapshot graph management.
- Provide runtime primitives needed by the parent system for safe live
  snapshot operations.
- Avoid storage waste from long snapshot chains through parent-managed
  policies, compaction, deduplication, and garbage collection.
- Keep the image model compatible with future Docker/git-like `commit`,
  `push`, and `pull` workflows.

## Non-Goals

- Scorpi runtime will not manage snapshot names, branches, tags, or refs.
- Scorpi runtime will not implement retention policy.
- Scorpi runtime will not decide when to compact.
- Scorpi runtime will not perform graph-level garbage collection.
- Scorpi runtime will not push or pull remote image repositories.
- Scorpi runtime will not store or interpret remote repository manifests.
- Scorpi runtime will not modify VM metadata when snapshots are created.
- Writable qcow2 overlay support is not required for the first version.

## Product Boundary

The Scorpi runtime should be a consumer of a prepared local image state.

Runtime responsibilities:

- open the configured top image
- probe the image format
- resolve the local backing chain
- validate that the chain is safe to use
- serve guest reads through the chain
- send guest writes to the writable top image
- flush and drain disk I/O when requested
- reopen or rebind a disk when requested by the parent system
- reject unsupported or unsafe image features

Parent management responsibilities:

- create snapshot overlays
- seal committed layers
- select the active top image for a VM
- maintain snapshot graph metadata
- maintain names, branches, commits, tags, and refs
- enforce retention policy
- perform compaction
- perform deduplication and garbage collection
- coordinate push and pull with remote repositories
- prevent multiple writers to the same active image
- recover interrupted management operations after crashes

## VM Configuration Requirements

The default VM disk configuration should remain simple:

```yaml
devices:
  pci:
    - device: virtio-blk
      slot: 2
      path: /var/lib/scorpi/images/vm1-active.sco
```

Scorpi should treat `path` as the top image. It should not require the VM
configuration to list the full backing chain.

The format should be resolved automatically:

```text
vm1-active.sco
  backs snap-003.sco
    backs snap-002.sco
      backs base.qcow2
```

The VM configuration should not need to change after a parent-managed snapshot
operation if the top image path remains the active image entry point.

Explicit format overrides may be allowed later for diagnostics or ambiguous
legacy cases, but normal operation should use probing.

## Runtime Image Requirements

Each image format driver should expose enough metadata for Scorpi to resolve
and validate the chain:

- virtual disk size
- logical sector size
- cluster or allocation granularity, when applicable
- readonly or writable capability
- parent location URI, when applicable
- feature flags
- whether the image is sealed or mutable, when applicable

Format detection should prefer image magic or structured headers. If no known
format is detected, Scorpi may fall back to raw.

Raw images cannot carry backing metadata and therefore act as base images.

Qcow2 should initially be supported as a base or readonly backing format. Scorpi
must reject unsupported qcow2 features rather than attempting best-effort I/O.

A Scorpi-native overlay format should be considered for snapshot layers. It
should be sparse, chunked, and simple enough to support efficient local I/O and
future content-addressed distribution.

## Chain Resolution Requirements

When opening a disk, Scorpi should:

1. Open the configured top image.
2. Probe its format.
3. Read backing metadata from the image driver.
4. Resolve parent location URIs according to their URI scheme.
5. Repeat until an image has no parent location URI.
6. Build an in-memory ordered chain from top image to base image.
7. Validate the complete chain before accepting guest I/O.

The first supported parent location scheme should be `file:`.

Examples:

```text
file:///var/lib/scorpi/images/base.sco
file:../base/base.sco
```

Future schemes such as `https:`, `s3:`, or `scorpi:` may be supported by a
future runtime extension or by parent-managed materialization into local
`file:` images. Unsupported schemes must fail closed.

Scorpi should validate:

- no cycles
- maximum chain depth is not exceeded
- every backing file exists and is readable
- all layers report compatible virtual disk sizes
- all layers report compatible sector sizes
- only the top layer is writable
- all lower layers are opened readonly
- unsupported format features are rejected
- parent location URI resolution is deterministic
- opened files are protected against unsafe concurrent mutation

Chain resolution should happen at disk open or explicit reopen time, not on
every I/O request.

## Runtime I/O Requirements

Guest reads should resolve data from the first layer in the chain that contains
the requested range. If no layer contains the range, the result should be zero
filled.

Guest writes should go only to the writable top image.

Guest flushes should flush the writable top image and any metadata needed to
make written data durable.

Guest discard or trim must not accidentally reveal data from a lower layer.
Overlay formats should represent discarded ranges with tombstones or equivalent
metadata so reads return zeroes instead of falling through to stale parent
content.

Scorpi should use allocation-map and resolved-range caches so long chains do
not require repeated linear traversal for hot reads.

## Live Snapshot Runtime Primitives

The parent management system may implement live snapshot operations. Scorpi
should expose the runtime primitives needed for this without understanding
snapshot policy.

Required primitives:

- pause or block new disk requests for a device
- drain outstanding disk requests
- flush guest-visible writes
- close, reopen, or rebind a disk to the current top image
- resume disk requests
- report errors without corrupting the old active image

A parent-managed live commit should be able to follow this shape:

```text
pause or quiesce guest/device I/O
drain block queues
flush the active image
seal the current writable overlay
create a new writable overlay above it
ask Scorpi to reopen the configured top image
resume guest/device I/O
```

Guest filesystem consistency is a parent-level concern. The parent system may
use guest agents, VM pause, filesystem freeze, or application-specific
coordination. Scorpi only needs to provide the block-level safety primitives.

## Snapshot Graph Requirements

The parent management system should model snapshots as a graph, not only as a
single chain.

Example:

```text
base
  -> install-tools
    -> app-server
      -> active
    -> database-server
      -> active
```

The parent graph must support:

- named user snapshots
- commit-like snapshots
- branches
- tags
- active VM roots
- remote refs
- temporary checkpoints
- reachability-based garbage collection
- in-progress operation markers

Scorpi runtime should only see the linear chain reachable from the chosen top
image.

## Commit, Push, And Pull Requirements

The future user experience may include Docker/git-like commands:

```text
scorpi commit vm1 repo/app:dev
scorpi push repo/app:dev
scorpi pull repo/app:prod
```

This does not change the Scorpi runtime boundary, but it does affect image and
metadata requirements.

Committed layers should be immutable. Published layers should never be mutated
in place. If compaction changes the representation, it should create new
layers and a new manifest or tag.

Remote identity should be content-addressed, not path-addressed. Local paths
are cache locations; repository identity should use digests and tags.

Remote manifests should belong to the parent management system, not the Scorpi
runtime. The parent system should materialize a local top image and backing
chain before launching Scorpi.

The image format should leave room for:

- per-layer digest metadata
- parent digest metadata
- per-chunk checksums
- sparse chunks
- zero-chunk optimization
- future partial or lazy pull
- deterministic serialization where practical

## Storage Efficiency Requirements

Long chains can waste storage and increase read amplification. The parent
management system must include policies and accounting to control this.

The image store should track:

- chain depth
- total referenced bytes
- unique bytes per layer
- bytes shared by multiple refs
- bytes overwritten by descendants
- bytes hidden by discard tombstones
- active writable layer size
- read amplification estimates
- pinned layers and refs

Recommended policy defaults:

- warn when a chain exceeds a moderate depth, such as 8 layers
- schedule compaction when a chain exceeds a higher depth, such as 12 layers
- require maintenance or refuse further automatic checkpointing at a hard
  maximum, such as 32 layers
- commit or rotate the active layer when it exceeds a configured size, age, or
  extent-count threshold

Exact thresholds should be configurable and validated during detailed design.

Compaction should create new layers instead of mutating old layers in place.
This is especially important for committed, tagged, or pushed layers.

For a linear chain:

```text
base -> a -> b -> c -> active
```

The parent may create:

```text
base -> compacted-abc -> active
```

For a branched graph, compaction should preserve sharing and avoid duplicating
large shared ancestors into every branch.

Garbage collection must be reachability-based. Roots include:

- active VM top images
- named snapshots
- tags
- local refs
- remote refs retained locally
- in-progress operations
- files currently opened by a running Scorpi process

Unreachable layers should be deleted only after the parent system knows no
running VM still has them open.

## Image Format Recommendations

The first Scorpi-native overlay format should prioritize correctness and
operational simplicity.

Recommended properties:

- fixed-size clusters, for example 64 KiB or 256 KiB
- sparse allocation
- allocation map with efficient lookup
- zero-range representation
- discard tombstone representation
- parent location URI
- optional parent digest
- sealed/immutable flag
- feature flags
- metadata checksum
- per-cluster checksum, at least for sealed layers

The format should avoid hidden mutable global state so readonly layers can be
cached, copied, pushed, pulled, and verified.

## Safety Requirements

Scorpi and the parent management system must avoid unsafe image mutation.

Required safety properties:

- a writable active image must have only one writer
- lower layers in a running chain must be immutable or treated as immutable
- the parent must not compact or rewrite an image file while Scorpi has it open
- live layer swaps must use explicit Scorpi drain/flush/reopen primitives
- interrupted parent operations must be recoverable
- metadata updates must be atomic from the parent system's point of view
- Scorpi should fail closed on unsupported or inconsistent image metadata

Security-sensitive behavior:

- imported qcow2 backing locations must not be trusted blindly
- parent location URIs must be resolved predictably
- path traversal and unexpected absolute paths should be governed by parent
  policy
- unsupported encryption, compression, or external data-file features should
  be rejected until explicitly supported

## Compatibility Requirements

Existing simple disk configuration must continue to work:

```yaml
- device: virtio-blk
  path: ./disk.raw
```

Existing ISO and readonly disk use cases must continue to work:

```yaml
- device: virtio-blk
  path: ./installer.iso
  ro: true
```

The block device emulations should not need snapshot-specific logic. Snapshot
and image-chain behavior should sit behind the block interface used by virtio
blk and AHCI.

## Observability Requirements

Scorpi should expose enough information for the parent system to diagnose disk
state:

- resolved chain
- detected format per layer
- readonly/writable status per layer
- virtual size and sector size
- chain depth
- unsupported feature errors
- current disk open generation, if used
- pending I/O drain status during live operations

The parent management system should expose higher-level storage diagnostics:

- snapshot graph
- refs and tags
- per-layer space usage
- unique and shared bytes
- compaction candidates
- garbage collection candidates
- push/pull status

## Open Questions

- Should Scorpi-native overlays be stored as single files or as a directory of
  metadata plus chunk files?
- What cluster size should the first implementation use?
- What qcow2 feature subset should be accepted for readonly backing images?
- Should raw fallback require explicit opt-in for security-sensitive import
  paths?
- What is the exact maximum chain depth for the runtime?
- Should the parent preserve a stable top image path by rewriting image
  metadata, replacing a symlink, or using a small pointer file?
- How should file locking work across Scorpi processes and parent management
  operations?
- Should live snapshot support require guest pause in the first version, or is
  block-level drain/flush enough?
- What metadata store should the parent management system use for snapshot
  graph state?
- How should remote manifests map to local parent location URIs after pull?

## Proposed Phases

### Phase 1: Runtime Image Abstraction

- Refactor the block layer to support image backend drivers.
- Preserve existing raw-file behavior.
- Add format probing.
- Add chain resolution and validation framework.

### Phase 2: Base Format Support

- Support raw as a base image.
- Support readonly qcow2 as a base or lower backing image.
- Reject unsupported qcow2 features clearly.

### Phase 3: Scorpi Overlay

- Define and implement the initial Scorpi overlay format.
- Support read-through chains.
- Support writes to a single top writable overlay.
- Support discard tombstones or equivalent zero-range semantics.

### Phase 4: Runtime Control Primitives

- Add disk pause/drain/flush/reopen support.
- Expose those primitives through the library/control boundary.
- Validate live top-image replacement against running I/O.

### Phase 5: Parent Snapshot Management

- Implement parent-managed snapshot graph metadata.
- Implement commit-like snapshots.
- Implement refs, tags, and active VM roots.
- Implement reachability-based garbage collection.

### Phase 6: Storage Optimization

- Implement accounting for unique, shared, overwritten, and dead bytes.
- Implement chain-depth policies.
- Implement offline compaction.
- Add branch-aware compaction.

### Phase 7: Remote Distribution

- Define remote manifests.
- Add content-addressed layer identity.
- Add push and pull.
- Add verification by digest.
- Leave room for lazy or partial pull.

## Success Criteria

- A VM can boot from a raw disk without snapshot metadata.
- A VM can boot from a qcow2 base image when the qcow2 feature set is
  supported.
- A VM config can point only at a top overlay image and Scorpi can resolve the
  full local backing chain automatically.
- Reads through a long chain return correct data.
- Writes affect only the top writable image.
- Discard does not reveal stale parent data.
- Scorpi rejects invalid chains, cycles, incompatible sizes, and unsupported
  features.
- The parent system can create a commit-like snapshot without modifying the VM
  config.
- The parent system can compact local chains without mutating published layers.
- The parent system can account for storage waste and identify compaction
  candidates.
- The architecture remains compatible with future push/pull workflows.
