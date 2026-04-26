# Scorpi Snapshot Implementation Plan

## Purpose

This document turns the snapshot PRD and detailed design into an implementation
task list.

Source documents:

- [snapshot_prd.md](snapshot_prd.md)
- [snapshot_image_format_research.md](snapshot_image_format_research.md)
- [snapshot_detailed_design.md](snapshot_detailed_design.md)

Each task has:

- scope
- dependencies
- implementation notes
- validation criteria

A task is not complete until its validation criteria pass.

## Execution Rules

- Keep runtime image support separate from parent snapshot management.
- Preserve existing raw disk behavior while introducing the new image stack.
- Prefer additive changes behind `block_if` before changing device emulations.
- Keep each task independently buildable and testable.
- Do not implement compaction, push/pull, or repository behavior in the Scorpi
  runtime.
- Validation must be runnable from the repo, not asserted only in prose.

## Validation Levels

- Build validation
  - Meson reconfigure succeeds.
  - Required targets compile.
- Unit validation
  - New unit tests compile and pass.
- I/O validation
  - Image reads, writes, flushes, and discards return expected data.
- Crash validation
  - Interrupted metadata updates recover to a valid generation.
- Integration validation
  - Existing virtio-blk and AHCI paths continue to work through `block_if`.

## Task List

### Task 0: Confirm Runtime Boundary

Status:

- Done

Scope:

- link this implementation plan to the snapshot PRD and detailed design
- confirm Scorpi runtime responsibilities vs parent management responsibilities
- document that Scorpi opens only a configured top image path and resolves the
  local backing chain

Dependencies:

- none

Implementation notes:

- no runtime code changes are required for this task
- this task is the checkpoint before implementation starts
- the runtime boundary is:
  - Scorpi opens the configured top image path
  - Scorpi probes the top image format
  - Scorpi resolves the local backing chain through image metadata
  - Scorpi validates and serves local block I/O
  - Scorpi exposes drain, flush, and reopen primitives
- the parent management boundary is:
  - snapshot graph metadata
  - commit names, refs, branches, and tags
  - compaction
  - retention and garbage collection
  - push, pull, and repository manifests
  - remote materialization into local `file:` image chains

Validation criteria:

- source documents are linked above
- design docs describe the same runtime boundary
- task plan does not assign graph management, compaction, GC, push, or pull to
  Scorpi runtime tasks
- implementation can proceed to Task 1 without deciding parent graph schema

### Task 1: Add Internal Image Backend Interface

Status:

- Done

Scope:

- add internal image backend types and ops
- add backend registration/probing helpers
- add internal `scorpi_image_info` and `scorpi_image_extent` equivalents
- keep the interface private to runtime code for now

Dependencies:

- Task 0

Implementation notes:

- start with enough ops for raw:
  - probe
  - open
  - get info
  - map
  - read
  - write
  - discard
  - flush
  - close
- do not expose this as public API yet
- avoid changing virtio-blk or AHCI behavior in this task

Validation criteria:

- build succeeds
- a unit test can register/probe a fake backend
- no behavior change for existing raw disk launch paths

Validation performed:

- `meson setup builddir --reconfigure`
- `meson test -C builddir scorpi_image_backend_test`
- `meson compile -C builddir`

### Task 2: Move Current Raw File I/O Behind Raw Backend

Status:

- Done

Scope:

- implement a raw image backend that preserves current flat-file behavior
- make raw report a single fully present layer
- keep existing `path`, `ro`, `sync`, `direct`, `sectorsize`, and
  `bootindex` behavior

Dependencies:

- Task 1

Implementation notes:

- raw has no parent location URI
- raw virtual size is file size
- raw `map()` reports `PRESENT`
- raw writable support remains available for legacy non-snapshot disks
- keep `blockif_open()` external behavior unchanged

Validation criteria:

- existing block tests pass
- existing sample raw/ISO configurations still launch to the same point as
  before
- unit tests verify raw read, write, flush, readonly rejection, and size
  reporting

Validation performed:

- `meson setup builddir --reconfigure`
- `meson test -C builddir scorpi_image_backend_test scorpi_image_raw_test`
- `meson compile -C builddir`

### Task 3: Add Image Chain Object

Status:

- Done

Scope:

- add `scorpi_image_chain` or equivalent
- represent ordered layers from top image to base image
- route `block_if` requests through a single-layer chain for raw

Dependencies:

- Task 2

Implementation notes:

- start with one-layer raw chains only
- add chain-level read/write/flush/discard entry points
- writes go to layer 0
- reads resolve through the chain API even when there is only one layer

Validation criteria:

- raw behavior remains unchanged
- unit tests verify one-layer chain read/write/flush
- no device emulation code needs snapshot-specific changes

Validation performed:

- `meson setup builddir --reconfigure`
- `meson test -C builddir scorpi_image_backend_test scorpi_image_raw_test scorpi_image_chain_test`
- `meson compile -C builddir`

### Task 4: Add Format Probing And Raw Fallback Policy

Status:

- Done

Scope:

- implement backend probing by file content
- add stable ordering and probe scores
- add configurable raw fallback behavior

Dependencies:

- Task 3

Implementation notes:

- raw fallback should be explicit inside the resolver because raw has no magic
- local CLI may default to raw fallback for compatibility
- future parent daemon may disable raw fallback by policy
- extension should not determine format

Validation criteria:

- known raw file opens through raw fallback
- unknown file opens as raw only when raw fallback is enabled
- unknown file is rejected when raw fallback is disabled
- fake backend with magic wins over raw fallback

Validation performed:

- `meson setup builddir --reconfigure`
- `meson test -C builddir scorpi_image_backend_test scorpi_image_open_test scorpi_image_raw_test scorpi_image_chain_test`
- `meson compile -C builddir`

### Task 5: Implement Parent Location URI Parser

Status:

- Done

Scope:

- parse parent location URIs
- support v1 `file:` scheme
- reject unsupported schemes
- resolve relative `file:` URIs relative to the child image directory

Dependencies:

- Task 4

Implementation notes:

- support:
  - `file:///absolute/path/to/parent.sco`
  - `file:relative/path/to/parent.sco`
- store both original URI and resolved path for diagnostics
- enforce path policy hooks, even if initially permissive

Validation criteria:

- unit tests cover absolute `file:` URI
- unit tests cover relative `file:` URI
- unit tests reject unsupported schemes such as `https:`
- unit tests cover path traversal policy behavior

Validation performed:

- `meson setup builddir --reconfigure`
- `meson test -C builddir scorpi_image_uri_test scorpi_image_backend_test scorpi_image_open_test scorpi_image_raw_test scorpi_image_chain_test`
- `meson compile -C builddir`

### Task 6: Add Backing Chain Resolver

Status:

- Done

Scope:

- resolve parent chains from top image metadata
- build ordered top-to-base chain
- add cycle detection and depth limits

Dependencies:

- Task 5

Implementation notes:

- use file identity where available for cycle detection
- use canonical path as fallback
- default maximum chain depth should be 32
- raw remains terminal because it has no parent URI

Validation criteria:

- single raw file resolves as depth 1
- simple overlay-like fake backend resolves parent chain
- missing parent is rejected
- cycle is rejected
- chain above max depth is rejected

Validation performed:

- `meson setup builddir --reconfigure`
- `meson test -C builddir scorpi_image_uri_test scorpi_image_backend_test scorpi_image_open_test scorpi_image_raw_test scorpi_image_chain_test scorpi_image_chain_resolver_test`
- `meson compile -C builddir`

### Task 7: Add Chain Validation

Status:

- Done

Scope:

- validate complete resolved chains before accepting I/O
- enforce size, sector, readonly, and writable-layer rules

Dependencies:

- Task 6

Implementation notes:

- reject incompatible virtual sizes
- reject incompatible logical sector sizes
- reject multiple writable layers
- reject writable lower layers
- reject readonly top image for writable VM disks
- validate base UUID/digest when available

Validation criteria:

- tests reject size mismatch
- tests reject sector mismatch
- tests reject lower writable layer
- tests reject top readonly when write access is requested
- tests reject base identity mismatch

Validation performed:

- `meson setup builddir --reconfigure`
- `meson test -C builddir scorpi_image_uri_test scorpi_image_backend_test scorpi_image_open_test scorpi_image_raw_test scorpi_image_chain_test scorpi_image_chain_resolver_test`
- `meson compile -C builddir`

### Task 8: Add Chain Read Resolver And Cache

Status:

- Done

Scope:

- implement multi-layer read resolution
- add extent or cluster owner cache
- handle `PRESENT`, `ABSENT`, `ZERO`, and `DISCARDED`

Dependencies:

- Task 7

Implementation notes:

- reads walk from top to base only on cache miss
- `ZERO` and `DISCARDED` zero-fill and stop traversal
- absent ranges with no parent zero-fill
- cache can initially be cluster-index based

Validation criteria:

- read from top layer returns top data
- absent range falls through to base
- absent range without base returns zero
- zero/discard state prevents base fallthrough
- cache is invalidated on write/discard/reopen

Validation performed:

- `meson test -C builddir scorpi_image_uri_test scorpi_image_backend_test scorpi_image_open_test scorpi_image_raw_test scorpi_image_chain_test scorpi_image_chain_resolver_test`
- `meson compile -C builddir`

### Task 9: Specify Fixed `.sco` V1 On-Disk Layout

Status:

- Done

Scope:

- create a precise `.sco` format specification document or section
- define fixed offsets, struct sizes, alignment, endian rules, and checksums
- define `SCOIMG\0\0`, `format_major`, and `format_minor`

Dependencies:

- Task 0

Implementation notes:

- this task is documentation plus test-fixture planning
- define:
  - file identifier block
  - superblock A/B
  - base descriptor
  - map root
  - map pages
  - map entries
  - cluster data area
- define v1 cluster-size limits
- define v1 map-page size
- define feature bits
- define superblock generation selection rules

Validation criteria:

- format spec is reviewed and internally consistent
- fixture generator can be implemented from the spec without guessing field
  offsets
- compatibility rules are documented

Validation performed:

- created [sco_v1_format.md](sco_v1_format.md)
- linked the fixed format spec from [snapshot_detailed_design.md](snapshot_detailed_design.md)
- `git diff --check`

### Task 10: Implement `.sco` Parser And Superblock Selection

Status:

- Done

Scope:

- implement readonly parsing for `.sco` headers
- validate magic, version, feature flags, checksums, and superblock generation
- select newest valid superblock

Dependencies:

- Task 9

Implementation notes:

- unsupported major version fails closed
- unsupported incompatible feature fails closed
- higher minor version opens only when feature flags allow it
- if newest superblock is corrupt, fall back to older valid superblock

Validation criteria:

- valid `.sco` opens
- bad magic is rejected
- unsupported version is rejected
- unknown incompatible feature is rejected
- invalid checksum is rejected
- newest valid generation is selected
- corrupt newest generation falls back to previous valid generation

Validation performed:

- `meson setup builddir --reconfigure`
- `meson test -C builddir scorpi_image_uri_test scorpi_image_backend_test scorpi_image_open_test scorpi_image_raw_test scorpi_image_sco_test scorpi_image_chain_test scorpi_image_chain_resolver_test`
- `meson compile -C builddir`

### Task 11: Implement `.sco` Base Descriptor

Status:

- Done

Scope:

- parse base descriptor
- expose base URI, base UUID, and base digest
- integrate descriptor with chain resolver

Dependencies:

- Task 10
- Task 6

Implementation notes:

- base descriptor does not store authoritative base format, virtual size,
  or sector size
- resolver discovers base metadata by opening the base image
- base UUID/digest are validation data only

Validation criteria:

- `.sco` with no base resolves as terminal
- `.sco` with `file:` base resolves base
- unsupported base URI scheme is rejected
- base identity mismatch is rejected when identity fields are present

Validation performed:

- `meson test -C builddir scorpi_image_sco_test scorpi_image_chain_resolver_test scorpi_image_uri_test scorpi_image_open_test`
- `meson compile -C builddir`

### Task 12: Implement `.sco` Allocation Map Read Path

Status:

- Done

Scope:

- parse map root
- load map pages
- map virtual clusters to states and physical offsets
- support readonly `.sco` reads

Dependencies:

- Task 10

Implementation notes:

- missing map page means all clusters covered by that page are `ABSENT`
- map pages are checksummed
- one map page covers a fixed number of virtual clusters
- reads should split on cluster boundaries

Validation criteria:

- present cluster reads from `.sco`
- absent cluster falls through to parent
- zero cluster returns zeroes
- discarded cluster returns zeroes
- corrupt map page is rejected

Validation performed:

- `meson test -C builddir scorpi_image_uri_test scorpi_image_backend_test scorpi_image_open_test scorpi_image_raw_test scorpi_image_sco_test scorpi_image_chain_test scorpi_image_chain_resolver_test`
- `meson compile -C builddir`

### Task 13: Implement `.sco` Read-Through Chains

Status:

- Done

Scope:

- combine `.sco` read map with chain read resolver
- support multiple `.sco` layers over raw

Dependencies:

- Task 8
- Task 12

Implementation notes:

- no writes yet
- lower layers opened readonly
- chain diagnostics should show each layer

Validation criteria:

- `.sco -> raw` reads correct data
- `.sco -> .sco -> raw` reads correct data
- top zero/discard prevents parent fallthrough
- chain diagnostics report expected depth and formats

Validation performed:

- `meson test -C builddir scorpi_image_uri_test scorpi_image_backend_test scorpi_image_open_test scorpi_image_raw_test scorpi_image_sco_test scorpi_image_chain_test scorpi_image_chain_resolver_test`
- `meson compile -C builddir`

### Task 14: Implement `.sco` Creation Tooling For Tests

Status:

- Done

Scope:

- add a small internal fixture generator or test helper for `.sco` images
- generate deterministic tiny overlays
- avoid hand-maintaining binary fixtures where possible

Dependencies:

- Task 12

Implementation notes:

- helper can live under tests or tools
- support creating base overlay, base descriptor, present clusters, zero
  entries, discarded entries, and corrupt variants

Validation criteria:

- tests can generate `.sco` fixtures reproducibly
- generated fixtures pass parser tests
- corrupt fixtures fail as expected

Validation performed:

- `meson test -C builddir scorpi_image_uri_test scorpi_image_backend_test scorpi_image_open_test scorpi_image_raw_test scorpi_image_sco_test scorpi_sco_fixture_test scorpi_image_chain_test scorpi_image_chain_resolver_test`
- `meson compile -C builddir`

### Task 15: Add `scorpi-image` CLI Tool

Status:

- Done

Scope:

- add a Meson executable target for a user-facing `scorpi-image` utility
- support creating `.sco` images from command-line arguments
- support inspecting image metadata without launching a VM
- keep snapshot graph, compaction, repository, push, and pull behavior outside
  this tool

Dependencies:

- Task 14

Implementation notes:

- initial subcommands should be small and deterministic:
  - `create`
  - `info`
  - `check`
- `create` should support `.sco` output, virtual size, and optional
  positional base image
- `create` should use a fixed v1 cluster size for now
- `--size` should accept raw bytes and `mb`/`gb` suffixes
- `info` should use the same parser/probing path as runtime image opening
- raw input should remain explicit because raw has no magic
- the tool may reuse the test fixture creation code, but it should be built as
  a real executable target rather than a test-only helper
- this tool does not replace the parent management system; it only creates and
  inspects local image files

Validation criteria:

- `meson compile` builds the `scorpi-image` executable
- `scorpi-image create` creates a valid `.sco` image
- `scorpi-image create ... path [base]` records a base URI when base is
  present
- `scorpi-image info` reports format, virtual size, sector sizes, cluster size,
  readonly or sealed state, and base URI when present
- `scorpi-image check` rejects corrupt `.sco` metadata
- generated images can be opened by the runtime image backend tests

Validation performed:

- `meson test -C builddir scorpi_image_uri_test scorpi_image_backend_test scorpi_image_open_test scorpi_image_raw_test scorpi_image_sco_test scorpi_sco_fixture_test scorpi_image_cli_test scorpi_image_chain_test scorpi_image_chain_resolver_test`
- `meson compile -C builddir`

### Task 16: Implement `.sco` Writable Top Support

Status: Done

Scope:

- support writes to a writable top `.sco`
- allocate data clusters
- allocate map pages dynamically
- update map entries

Dependencies:

- Task 13

Implementation notes:

- writes only go to layer 0
- lower layers remain readonly
- map pages are allocated on demand
- map root entries for absent map pages mean all covered clusters are absent

Validation criteria:

- full-cluster write persists after reopen
- write to new map page allocates that page
- writes do not modify parent images
- readonly/sealed top rejects writes

Validation performed:

- `meson compile -C builddir`
- `meson test -C builddir scorpi_image_uri_test scorpi_image_backend_test scorpi_image_open_test scorpi_image_raw_test scorpi_image_sco_test scorpi_sco_fixture_test scorpi_image_cli_test scorpi_image_chain_test scorpi_image_chain_resolver_test`

### Task 16A: Make Image I/O Thread-Safe

Status: Done

Scope:

- support multiple block worker threads issuing reads and writes through the
  same image chain
- protect `.sco` metadata updates for direct backend users
- keep concurrent reads scalable where possible

Implementation notes:

- image chains use a read/write lock: reads take the shared side, while writes,
  discards, flushes, and close take the exclusive side
- the chain read cache has its own mutex so concurrent readers can share the
  chain read lock without racing cache state
- `.sco` backends use a read/write lock: map/read/info take the shared side,
  while write/flush/close take the exclusive side
- `.sco` write locking covers data cluster allocation, map page allocation,
  map entry updates, and root entry CRC updates as one in-process critical
  section
- locks are per opened image object; concurrent opens of the same file from
  different processes are not coordinated by this task

Validation criteria:

- concurrent chain readers do not race the read cache
- concurrent chain writes are serialized against reads
- concurrent direct `.sco` writes preserve all map updates
- existing image chain and `.sco` behavior remains unchanged

Validation performed:

- `meson test -C builddir scorpi_image_chain_test scorpi_image_sco_test`
- `meson test -C builddir scorpi_image_uri_test scorpi_image_backend_test scorpi_image_open_test scorpi_image_raw_test scorpi_image_sco_test scorpi_sco_fixture_test scorpi_image_cli_test scorpi_image_chain_test scorpi_image_chain_resolver_test`
- `meson compile -C builddir`

### Task 16B: Add Fine-Grained `.sco` Write Locking

Scope:

- reduce unnecessary serialization after `.sco` supports in-place overwrite
  paths
- distinguish metadata-changing writes from data-only writes
- avoid torn overlapping reads and writes

Dependencies:

- Task 16A
- in-place overwrite support

Implementation notes:

- keep the current exclusive image lock for metadata-changing writes:
  allocation, map page creation, map entry replacement, discard/zero state
  changes, and root CRC updates
- data-only overwrites may use a shared metadata lock plus an exclusive
  per-cluster or byte-range data lock
- reads may use a shared metadata lock plus a shared data-range lock when the
  range overlaps writable top data
- non-overlapping data-only overwrites should be able to run concurrently
- the first implementation may use fixed lock striping by cluster index instead
  of allocating one lock per cluster
- preserve the simple chain-level exclusive write lock until the backend range
  locking contract is explicit at the image API boundary

Validation criteria:

- concurrent non-overlapping overwrites do not serialize on the global `.sco`
  metadata lock
- overlapping read/write operations never observe torn cluster contents
- metadata-changing writes remain serialized with all reads and writes that
  need a consistent map view
- existing Task 16A thread-safety tests still pass

### Task 17: Implement Whole-Cluster Materialization

Status: Done

Scope:

- handle partial writes to absent clusters
- materialize previous chain-visible data before applying partial write
- overwrite already-present top clusters in place when no metadata change is
  required

Dependencies:

- Task 16

Implementation notes:

- v1 does not support subcluster allocation
- partial write to absent cluster reads full cluster from chain or zero-fills
- then patches guest write bytes and stores a full local cluster
- partial write to an already-present top cluster reads that local cluster,
  patches guest bytes, and writes it back to the same physical offset
- full-cluster write to an already-present top cluster writes directly to the
  existing physical offset
- in-place overwrites must not allocate a replacement data cluster or update
  map/root metadata
- absent, zero, discarded, or lower-layer-visible clusters still allocate a new
  local cluster before becoming present in the top image

Validation criteria:

- partial write preserves parent bytes before and after written range
- partial write with no parent zero-fills unwritten ranges
- write crossing cluster boundary materializes each affected cluster correctly
- overwrite of an already-present top cluster does not grow the `.sco` file
- overwrite of an already-present top cluster does not rewrite map/root metadata

Validation performed:

- `meson test -C builddir scorpi_image_sco_test scorpi_image_chain_test scorpi_image_chain_resolver_test`
- `meson test -C builddir scorpi_image_uri_test scorpi_image_backend_test scorpi_image_open_test scorpi_image_raw_test scorpi_image_sco_test scorpi_sco_fixture_test scorpi_image_cli_test scorpi_image_chain_test scorpi_image_chain_resolver_test`
- `meson compile -C builddir`

### Task 18: Implement `.sco` Discard And Zero State

Scope:

- support top-layer discard behavior
- prevent parent fallthrough after discard
- decide and implement zero-write optimization if needed

Dependencies:

- Task 16

Implementation notes:

- full-cluster discard marks cluster `DISCARDED`
- partial discard may materialize and zero range in v1
- advertise discard only when backend can persist the state safely

Validation criteria:

- discard over full cluster returns zero on later read
- discard over absent cluster does not reveal parent data
- partial discard preserves non-discarded bytes correctly
- `blockif_candelete()` reflects backend capability

### Task 19: Implement Crash-Safe `.sco` Metadata Commit

Scope:

- implement copy-on-write metadata update flow
- update inactive superblock with incremented generation
- recover newest valid generation on open

Dependencies:

- Task 16

Implementation notes:

- write data before publishing metadata
- write new map page versions before publishing new superblock
- fsync according to selected durability mode
- leaked unreferenced data after crash is acceptable in v1

Validation criteria:

- crash simulation: data written, metadata not committed
- crash simulation: map page written, superblock not committed
- crash simulation: inactive superblock partially written
- open selects newest valid generation
- old valid generation remains usable after interrupted write

### Task 20: Add `.sco` Flush Semantics

Scope:

- define and implement flush behavior for `.sco`
- ensure guest-visible completed writes survive flush

Dependencies:

- Task 19

Implementation notes:

- flush persists data clusters, map updates, discard/zero state, and active
  superblock generation
- readonly lower layers do not need flush

Validation criteria:

- flush followed by close/reopen preserves data
- flush errors are returned to block request completion
- tests cover no-space or injected flush failure where practical

### Task 20A: Add `scorpi-image` Seal And Unseal Commands

Scope:

- add explicit `scorpi-image seal path.sco` command
- add explicit `scorpi-image unseal path.sco` command
- update `.sco` readonly-compatible sealed state through a new superblock
  generation
- keep sealing as an image-state transition, not a `create` flag

Dependencies:

- Task 19
- Task 20

Implementation notes:

- `seal` sets the `.sco` readonly-compatible sealed bit
- `unseal` clears the sealed bit only for local images where policy allows it
- `unseal` should likely require `--force` once repository/published-layer
  policy exists
- both commands must use the same crash-safe metadata commit path as writable
  `.sco` updates
- both commands should reject unsupported image formats
- sealing should not change base descriptors, allocation maps, or data clusters

Validation criteria:

- `scorpi-image seal path.sco` marks the image sealed
- sealed image rejects writable opens
- `scorpi-image unseal path.sco` clears sealed state for allowed local images
- seal and unseal survive close/reopen
- interrupted seal/unseal leaves either the previous valid generation or the
  new valid generation usable

### Task 21: Add Resolved Chain Diagnostics

Scope:

- expose resolved chain metadata for debugging and parent integration
- include original URI and resolved local path where applicable

Dependencies:

- Task 13

Implementation notes:

- diagnostics should include:
  - path
  - format
  - readonly/writable
  - sealed state
  - virtual size
  - sector sizes
  - cluster size
  - chain depth
  - parent URI
  - resolved path
- initial exposure can be logs or internal test API

Validation criteria:

- tests verify diagnostics for raw
- tests verify diagnostics for `.sco -> raw`
- unsupported feature errors include enough context to identify the layer

### Task 22: Add Readonly Qcow2 Backend

Scope:

- implement minimal readonly qcow2 support
- support normal clusters, zero clusters, L1/L2 mapping, and backing metadata
- reject unsupported features

Dependencies:

- Task 4
- Task 8

Implementation notes:

- no writable qcow2 in this phase
- no encryption
- no compression
- no external data files
- internal snapshots are not exposed as Scorpi snapshot state
- unknown incompatible features fail closed

Validation criteria:

- simple readonly qcow2 reads correctly
- qcow2 zero clusters return zeroes
- qcow2 backing file resolves through chain resolver
- unsupported qcow2 feature is rejected
- qcow2 cannot be opened writable

### Task 23: Add Block Drain/Pause Primitives

Scope:

- add a way to pause new disk requests
- drain outstanding requests
- report when a disk is idle

Dependencies:

- Task 3

Implementation notes:

- build on existing block queue pause behavior if available
- behavior must be per block context
- no image reopen yet

Validation criteria:

- requests submitted before pause complete
- requests submitted after pause are blocked or rejected according to design
- drain reports completion when queues are empty
- resume allows requests again

### Task 24: Add Disk Reopen/Rebind Primitive

Scope:

- reopen the configured top image path
- resolve and validate new chain
- atomically swap chain object after drain

Dependencies:

- Task 23
- Task 7
- Task 13

Implementation notes:

- on reopen failure, keep old chain available and disk paused
- parent can repair or resume old chain
- drop chain read cache on successful reopen

Validation criteria:

- stopped or paused disk can reopen to new top `.sco`
- failed reopen does not corrupt old chain
- successful reopen observes new parent chain
- post-reopen reads and writes use new chain

### Task 25: Integrate Reopen With Control/Library Boundary

Scope:

- expose disk pause/drain/flush/reopen through the appropriate runtime control
  path
- keep parent snapshot policy outside runtime

Dependencies:

- Task 24

Implementation notes:

- exact API shape depends on current control-channel/library refactor state
- API should target disk identity, not snapshot identity
- report detailed errors

Validation criteria:

- parent-style test can pause, flush, swap top path, reopen, and resume
- API does not accept snapshot names or graph refs
- errors are reported without process termination

### Task 26: Add Parent Image Store Design

Scope:

- write detailed design for the parent-managed image graph
- define image node metadata, refs, active roots, tags, and pins

Dependencies:

- Task 0

Implementation notes:

- this is outside Scorpi runtime
- design should consume runtime diagnostics and reopen primitives
- include crash recovery for parent operations

Validation criteria:

- design defines graph schema
- design defines active VM root ownership
- design defines locking and recovery model

### Task 27: Implement Parent Commit/Snapshot Prototype

Scope:

- implement first parent-managed commit operation
- seal current `.sco`
- create new writable `.sco`
- preserve or update stable top image reference

Dependencies:

- Task 25
- Task 26

Implementation notes:

- stopped VM flow can be implemented before live VM flow
- parent owns snapshot names and refs
- Scorpi runtime sees only local image files

Validation criteria:

- stopped VM commit creates sealed layer and new writable active layer
- VM config does not need to list backing chain
- live prototype can pause/flush/reopen using runtime primitives

### Task 28: Add Parent Storage Accounting

Scope:

- track storage metrics needed for compaction policy
- report chain depth, active layer size, unique bytes, shared bytes, and
  discard-hidden bytes

Dependencies:

- Task 26

Implementation notes:

- use `.sco` allocation maps for accounting
- runtime may expose per-layer allocated bytes
- exact policy thresholds can remain configurable

Validation criteria:

- parent can report chain depth per active VM
- parent can identify large active layers
- parent can identify candidate private tails for compaction

### Task 29: Implement Offline Compaction Prototype

Scope:

- compact a selected linear chain into a new layer
- avoid mutating open or published layers

Dependencies:

- Task 28

Implementation notes:

- start with stopped VM or offline images only
- create new `.sco` layer from resolved chain view
- atomically update parent-owned refs after successful compaction

Validation criteria:

- compacted chain reads the same data as original chain
- original layers remain unchanged
- parent refs update only after successful new layer creation

### Task 30: Add Garbage Collection

Scope:

- implement reachability-based cleanup in the parent system
- protect active roots, named snapshots, tags, remote refs, and open files

Dependencies:

- Task 26
- Task 29

Implementation notes:

- GC is parent-managed only
- runtime can help by reporting open layer paths

Validation criteria:

- unreachable layer is deleted only when not open
- reachable layer is preserved
- in-progress operations protect temporary files

### Task 31: Add Repository Manifest Design

Scope:

- design content-addressed manifests for future push/pull
- define layer digests, tags, and local materialization

Dependencies:

- Task 26

Implementation notes:

- manifests are not consumed directly by Scorpi runtime
- parent materializes local `file:` image chains before launch
- leave room for future lazy pull

Validation criteria:

- manifest design maps to local `.sco` layers
- tag vs digest semantics are documented
- parent URI handling remains compatible with local materialization

### Task 32: Add Push/Pull Prototype

Scope:

- prototype parent-managed push/pull of sealed `.sco` layers
- verify digests
- materialize pulled layers into local file-backed chains

Dependencies:

- Task 31

Implementation notes:

- no Scorpi runtime changes should be required
- pushed layers must be immutable
- pulled manifests become local image files before VM launch

Validation criteria:

- push uploads sealed layers and manifest
- pull verifies digests and creates local image files
- Scorpi can open the pulled local top image through `file:` URIs

## Suggested Implementation Order

Start with the runtime foundation:

1. Tasks 1-8: backend abstraction, raw backend, chain resolver, validation,
   read resolver.
2. Tasks 9-15: `.sco` readonly parser, map reads, fixtures, CLI creation.
3. Tasks 16-20: `.sco` writable top, materialization, discard, crash safety,
   flush.
4. Task 22: readonly qcow2.
5. Tasks 23-25: live drain/reopen primitives.

Then move to parent-managed behavior:

1. Tasks 26-28: parent graph and accounting.
2. Tasks 29-30: compaction and GC.
3. Tasks 31-32: repository manifests and push/pull.

## First Milestone Recommendation

The first useful milestone should be:

- raw behavior preserved through the new image backend stack
- `.sco -> raw` readonly chain works
- `.sco` writable top supports writes, flush, and reopen
- invalid chains are rejected
- tests cover partial writes and discard parent-fallthrough prevention

This milestone proves the core design without requiring qcow2, live reopen, or
parent-managed graph features.
