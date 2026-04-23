# Scorpi Kit Refactor Design

## Summary

This refactor should distinguish between:

- the current implementation target
  - `scorpi_kit`: a library that builds, configures, and runs VMs
  - `scorpi`: a thin local executable that uses `scorpi_kit`
- the future Scorpi product
  - a daemon exposing Docker-like APIs
  - one or more clients over that daemon API

This document is about the first part. The future product should influence the boundaries, but it is not part of the current implementation phase.

The library must support two equivalent entry paths:

- programmatic VM construction through C APIs
- declarative VM construction from a YAML document

The current codebase is already close to this shape in one important way: most device code is configured through an internal config tree and initialized later. The main gap is that configuration assembly, process lifecycle, and CLI parsing are still mixed together in the executable path.

This document proposes a refactor that preserves the existing device implementations while moving orchestration into a reusable library.

For the current implementation phase, examples in this document should prefer YAML files and `scorpi -f vm.yaml`. The C API remains part of the library contract, but YAML should be the primary documented way to describe a VM.

## Terminology

- `scorpi_kit`: embeddable library for VM spec creation, validation, instantiation, and runtime control
- `scorpi`: local executable that directly uses `scorpi_kit` in the current phase
- future Scorpi product: a daemon-centered system with Docker-like APIs, persistent VM inventory, snapshots, stats, and separate clients

`scorpi` is not the future product architecture. It is only one consumer of `scorpi_kit`.

## Current State

Today, Scorpi is built as one executable in [meson.build](/scorpi/meson.build). The flow is:

1. `main()` in [src/bhyverun.c](scorpi/src/bhyverun.c) initializes defaults and parses CLI arguments.
2. CLI parsing populates a global config tree via [src/config.c](scorpi/src/config.c).
3. Platform and device initialization consume that config tree.
4. The executable starts the VM and enters the event loop.

Important characteristics of the current design:

- Configuration is stored as a global `nvlist` tree behind `config.c`.
- PCI and USB devices are registered via linker-set style registries and created from config nodes.
- Device modules already mostly depend on `nvlist_t *nvl` and not on CLI arguments directly.
- The public API in [include/scorpi.h](scorpi/include/scorpi.h) and [src/libscorpi/scorpi.c](scorpi/src/libscorpi/scorpi.c) is only a placeholder today.

This means the best refactor is not to rewrite device code first. The best refactor is to move config assembly and lifecycle control into a real library, then let both YAML and CLI feed the same internal VM spec.

## Goals

- Provide a supported public library API for embedding Scorpi.
- Make YAML and API-based VM creation produce the same VM layout.
- Turn `scorpi` into a small local frontend over the library.
- Keep existing device modules usable during the migration.
- Reduce reliance on global process-wide state at the public API boundary.
- Keep the design compatible with a future daemon and separate clients.

## Non-Goals

- Rewriting every device to a new typed config interface in the first phase.
- Changing device behavior or supported VM features as part of this split.
- Designing a remote management protocol in this work.
- Implementing the future daemon in this phase.
- Implementing Linux/KVM support in this phase.

## Design Principles

- One canonical internal representation of a VM definition.
- One runtime path to create and launch a VM.
- CLI and YAML are input adapters, not separate implementations.
- Public APIs should not expose `nvlist_t`, linker sets, or platform-specific internals.
- Existing config-tree consumers should remain valid behind an adapter during migration.
- The local `scorpi` executable and the future daemon must sit above the same library boundary.

## Proposed Architecture

### Layers

#### 1. `scorpi_kit` public layer

This is the new supported library surface.

Responsibilities:

- create and destroy VM specifications
- configure VM properties through typed APIs
- add devices and backends
- load a spec from YAML
- validate a spec
- instantiate and run a VM
- expose lifecycle operations such as start, wait, stop, and destroy

This layer must remain usable both by the local `scorpi` executable and by future higher-level control-plane code.

#### 2. `scorpi` local executable

This is the current standalone launcher, not the future product control plane.

Responsibilities:

- parse CLI arguments
- build or load a `scorpi_kit` spec
- call `scorpi_kit` directly
- present local-user-oriented status and errors

`scorpi` should not own long-term VM inventory, snapshot catalogs, or Docker-like API semantics.

#### 3. `scorpi_kit` internal adapter layer

This layer converts a typed VM specification into the current internal config tree and startup sequence.

Responsibilities:

- translate typed spec objects into config nodes
- populate the existing config tree
- call the current initialization sequence now rooted in `main()`
- centralize error reporting and cleanup

This adapter is the key to a safe migration. It allows the public API to become clean without forcing all device code to change immediately.

#### 4. Runtime/device layer

This is the existing Scorpi core:

- platform initialization
- VMM/HVF interactions
- memory setup
- PCI/USB device creation
- network backends
- event loop

Most files under `src/` stay here with minimal early changes.

#### 5. Future product layer

This is out of scope for the current implementation, but the library must not block it.

Expected future pieces:

- a background daemon
- Docker-like control APIs
- persistent VM metadata and inventory
- snapshot and stats tracking
- client tools such as `scorpi`, `scorpi view`, and other admin commands

The future daemon should consume `scorpi_kit`; it should not replace it.

## Target Repository Layout

One reasonable target layout is:

```text
include/
  scorpi_kit/
    scorpi_kit.h
    scorpi_kit_yaml.h
    scorpi_kit_errors.h
src/
  scorpi_kit/
    api.c
    spec.c
    spec_validate.c
    yaml.c
    launch.c
    config_adapter.c
    error.c
  scorpi_runtime/
    ...existing runtime and device code...
  cli/
    main.c
  daemon/
    ...future work, not part of this phase...
```

Notes:

- The current `src/bhyverun.c` flow should be split so launch logic moves into library-callable functions.
- The existing `include/scorpi.h` should either be replaced by `include/scorpi_kit/scorpi_kit.h` or kept only as a temporary compatibility shim.
- The current `src/libscorpi/` placeholder should become the actual library implementation or be replaced by `src/scorpi_kit/`.

## Public API Shape

The API should be mixed:

- typed for the main object kinds such as VM and device
- generic for device-level properties

In particular, the public API should use one consistent `scorpi_*` naming family. It should be able to build exactly the same VM shape that the YAML format can describe.

Device configuration should not require a separately compiled typed struct for every device flavor. Properties such as `fb` should remain generic key/value properties attached to a device object. CPU count and memory size should be VM properties, but the API should still support future CPU and memory subproperties via dotted property names such as `cpu.cores`, `cpu.pinning`, `memory.size`, and `memory.wired`.

For the common case, the public API should also provide explicit convenience setters for CPU count and RAM size. Those should be thin wrappers over the same VM property model used by YAML normalization.

As a rule, every fallible public API should return `scorpi_error_t`. Only operations that cannot fail, such as object destruction, should return plain values or `void`.

### Core objects

- `scorpi_vm_t`: VM builder and runtime handle
- `scorpi_device_t`: device configuration object
- `scorpi_error_t`: structured error object

### Core lifecycle

Suggested high-level API:

```c
typedef struct scorpi_vm scorpi_vm_t;
typedef struct scorpi_device scorpi_device_t;
typedef struct scorpi_error scorpi_error_t;

scorpi_error_t scorpi_create_vm(scorpi_vm_t **out_vm);
void scorpi_destroy_vm(scorpi_vm_t *vm); /* does not fail */

scorpi_error_t scorpi_vm_set_prop_string(
    scorpi_vm_t *vm,
    const char *name,
    const char *value);
scorpi_error_t scorpi_vm_set_prop_bool(
    scorpi_vm_t *vm,
    const char *name,
    bool value);
scorpi_error_t scorpi_vm_set_prop_u64(
    scorpi_vm_t *vm,
    const char *name,
    uint64_t value);
scorpi_error_t scorpi_vm_set_cpu(
    scorpi_vm_t *vm,
    uint64_t cores);
scorpi_error_t scorpi_vm_set_ram(
    scorpi_vm_t *vm,
    uint64_t bytes);

scorpi_error_t scorpi_create_pci_device(
    const char *device,
    uint64_t slot, /* use SCORPI_PCI_SLOT_AUTO when unspecified */
    scorpi_device_t **out_dev);
scorpi_error_t scorpi_create_usb_device(
    const char *device,
    scorpi_device_t **out_dev);
scorpi_error_t scorpi_create_lpc_device(
    const char *device,
    scorpi_device_t **out_dev);
void scorpi_destroy_device(scorpi_device_t *dev); /* does not fail */

scorpi_error_t scorpi_device_set_prop_string(
    scorpi_device_t *dev,
    const char *name,
    const char *value);
scorpi_error_t scorpi_device_set_prop_bool(
    scorpi_device_t *dev,
    const char *name,
    bool value);
scorpi_error_t scorpi_device_set_prop_u64(
    scorpi_device_t *dev,
    const char *name,
    uint64_t value);
scorpi_error_t scorpi_vm_add_device(
    scorpi_vm_t *vm,
    scorpi_device_t *dev);

int scorpi_start_vm(scorpi_vm_t vm);
```

### YAML entry points

```c
scorpi_error_t scorpi_load_vm_from_yaml(
    const char *yaml,
    scorpi_vm_t **out_vm);
```

### Builder semantics

The API should follow the same construction model as YAML:

1. create a VM
2. set VM properties, including CPU and memory properties
3. create a device of the right bus type
4. set device properties, including hierarchy properties such as `parent`
5. add the device to the VM
6. start

`scorpi_start_vm()` should perform the final validation pass before launch rather than requiring a separate public validate API. It should run the VM in the caller process, return the VM exit code on success, and return a negative `SCORPI_ERR_*` value on launch/setup failure.

Example YAML file:

```yaml
name: vm1
cpu: 2
memory: 4G
bootrom: ./firmware/SCORPI_EFI.fd
bootvars: ./firmware/SCORPI_VARS.fd
devices:
  pci:
    - device: xhci
      slot: 1
      id: xhci0
  usb:
    - device: kbd
      parent: xhci0
  lpc:
    - device: vm-control
      path: /tmp/vm_sock
    - device: tpm
      type: swtpm
      path: /tmp/scorpi-tpm/swtpm.sock
      version: 2.0
      intf: tis
```

```text
scorpi -f vm.yaml
```

The optional PCI slot should be handled at creation time because slot selection is part of PCI placement semantics. If the caller does not want to choose a slot, it should pass a sentinel such as `SCORPI_PCI_SLOT_AUTO`.

The command socket and TPM should not be modeled as ad hoc VM globals in the API. They should be modeled as LPC-class devices, matching the YAML `devices.lpc` shape and keeping the API/YAML mapping consistent.

### Why not keep the current placeholder API

The current API sketch in `include/scorpi.h` is still not sufficient:

- unclear ownership
- no structured error-returning contract
- no explicit parent/child device relationships
- no consistent create/configure/add flow for devices
- no way to express CPU and memory subproperties as VM properties through the same builder model
- no explicit launch lifecycle

The convenience setters should coexist with the generic property API:

- `scorpi_vm_set_cpu(vm, 2)` should normalize to `cpu.cores = 2`
- `scorpi_vm_set_ram(vm, 4ULL * 1024 * 1024 * 1024)` should normalize to `memory.size`

YAML may still use friendly size syntax such as `memory: 4G`, but that should be parsed into the same internal numeric memory representation used by `scorpi_vm_set_ram()`.

Likewise:

- `scorpi_create_pci_device("virtio-net", 6, &dev)` should correspond to a PCI device entry with `slot: 6`
- `scorpi_create_pci_device("virtio-net", SCORPI_PCI_SLOT_AUTO, &dev)` should correspond to a PCI device entry with no explicit slot
- `scorpi_create_lpc_device("vm-control", &dev)` and `scorpi_create_lpc_device("tpm", &dev)` should correspond to YAML entries under `devices.lpc`

It is acceptable as an internal experiment, but it is not a good public ABI.

## Internal Spec Model

The typed spec should be the single source of truth. A VM spec should contain:

- VM identity
  - name
  - UUID
  - guest profile such as Linux or Windows
- CPU
  - cores
- Memory
  - size
- Firmware
  - boot ROM
  - boot vars
- Global options
  - ACPI on/off
  - MSI/MSI-X policy
  - VM-control socket
- Devices
  - PCI devices
  - USB devices
  - LPC-style devices such as TPM and VM-control

Each device entry should include:

- optional device `id`
- device name
- bus/slot/function or USB location if explicitly requested
- generic property map
- optional parent device reference, preferably by `id`

Generic properties are important for migration and forward compatibility. They let the library support existing options, new devices, and newly added device properties without forcing a new typed public struct or YAML schema revision for each change.

Guest profile should be modeled as a spec-level input for defaults and validation. It does not imply that Linux support is part of the current implementation phase.

The VM-control socket should be treated as a VM control channel, not just as a UI transport. It already carries keyboard and mouse injection, and it should also be the path for VM-directed commands such as shutdown. That makes it part of the runtime control model and future client/daemon boundary.

The `parent` field should be a generic device hierarchy reference. It should use a stable device `id` and not be limited to USB. USB devices can use it to point at a controller, and future topologies can use it for relationships such as devices behind a PCI bridge.

For USB devices specifically, if only one USB controller exists, the parent can be inferred. If more than one USB controller exists, each USB device should identify its parent controller explicitly by `id`.

The design should not depend on implicit names such as `xhci0` being auto-discovered by magic. If a config wants to reference a device reliably, it should assign that device an explicit `id`.

## YAML Format

YAML should map directly to the typed spec, not directly to the old config tree. At the device level, it should remain open-ended enough that new devices and new properties can be added without changing the top-level YAML grammar.

Example:

```yaml
name: vm1
cpu: 2
memory: 4G
bootrom: ./firmware/SCORPI_EFI.fd
bootvars: ./firmware/SCORPI_VARS.fd
devices:
  pci:
    - slot: 0
      id: host0
      device: hostbridge
    - slot: 1
      id: xhci0
      device: xhci
    - slot: 2
      id: ahci0
      device: ahci
      port.0.type: hd
      port.0.path: win11.img
      port.0.model: Windows Disk
      port.1.type: cd
      port.1.path: win.iso
      port.1.ro: true
      port.1.model: Windows Install ISO
    - slot: 5
      id: gpu0
      device: virtio-gpu
      fb: true
      edid: true
    - slot: 6
      id: net0
      device: virtio-net
      backend: "slirp"
      mac: "52:54:00:12:34:56"
  usb:
    - id: kbd0
      device: kbd
      parent: xhci0
    - id: tablet0
      device: tablet
      parent: xhci0
  lpc:
    - device: vm-control
      id: vmctl0
      path: /tmp/vm_sock
    - device: tpm
      id: tpm0
      type: swtpm
      path: /tmp/scorpi-tpm/swtpm.sock
      version: 2.0
      intf: tis
```

The explicit `id` is what makes references like `parent: xhci0` well-defined. Without that `id`, the parser should not guess a synthetic name unless the design explicitly defines a normalization rule and returns it back to the caller.

The same mechanism should work for future hierarchies as well, for example:

```yaml
devices:
  pci:
    - id: bridge0
      slot: 7
      device: pci-bridge
    - id: net1
      slot: 8
      device: virtio-net
      parent: bridge0
```

Example with implicit parent allowed because there is only one USB controller:

```yaml
devices:
  pci:
    - device: xhci
      slot: 1
  usb:
    - device: kbd
    - device: tablet
```

Expanded forms should remain valid when future properties are needed:

```yaml
memory: 4G
memory.wired: true
cpu: 2
cpu.pinning: "0,1"
```

Rules:

- YAML should be human-oriented and stable.
- Parsing should normalize friendly values such as `4G` into typed internal values.
- `memory: 4G` should be accepted as shorthand for `memory: { size: 4G }`.
- `cpu: 2` should be accepted as shorthand for `cpu: { cores: 2 }`.
- scalar shorthand and object form should normalize to the same internal spec.
- Validation should reject ambiguous or conflicting definitions before launch.
- device entries should be open property maps: unknown but syntactically valid properties should be preserved and passed through to runtime validation rather than rejected by the YAML parser itself.
- adding a new device or a new device property should not require a YAML format redesign.
- references between devices should use explicit `id` values when the relationship must be stable or unambiguous.

For `vm-control`, the YAML and API model should describe the endpoint as a named control channel rather than as a graphics-only or input-only socket.

For USB, parent controller selection may be implicit only when there is exactly one eligible controller in the VM spec. Otherwise `parent` should reference an explicit controller `id`.

More generally, `parent` should be treated as a generic hierarchy reference between devices, not as a USB-only field.

The parser should distinguish between:

- structural errors, which are YAML/spec format errors
- semantic errors, which are device/property validation errors reported after parsing

That distinction is important for forward compatibility. For example, a property such as `mac` or any additional device parameter should parse cleanly even if support is added later or provided by a different runtime build.

## Bridging Strategy

### Phase 1: spec to config-tree adapter

Implement:

- typed VM builder objects
- YAML loader
- validation
- adapter that writes into the existing config tree

This lets existing functions such as:

- `init_pci()`
- `init_tpm()`
- `netbe_init()`
- PCI and USB device init callbacks

continue to operate mostly unchanged.

In this phase:

- `scorpi_load_vm_*()` or `scorpi_create_vm()`-based builders populate config state
- existing runtime setup is extracted from `main()` into library-callable functions
- `scorpi` CLI becomes just another VM-definition producer

### Phase 2: runtime context instead of global config

Introduce an explicit runtime context:

- config tree becomes owned by a VM context instead of a process-global singleton
- launch functions take an explicit context/spec object
- library callers can safely create more than one VM sequentially in one process

This phase is necessary for a robust embedding story.

### Phase 3: prepare for future product consumers

Introduce boundaries that a future daemon will need:

- stable VM identifiers
- machine-readable error codes
- a VM state model such as created, running, stopped, failed
- stats and query hooks
- attach points for consoles and viewers
- a control channel for input injection and VM commands

The current `scorpi` executable may use only part of this surface, but it should not invent a separate private model.

### Phase 4: typed device configuration

Keep core VM configuration typed, but move device configuration toward:

- generic property maps
- runtime property discovery
- explicit parent/child relationships where needed, such as USB devices under xHCI controllers or devices attached behind a PCI bridge

The adapter remains as a compatibility layer for runtime/device code that still consumes the config tree.

## Required Refactors in Existing Code

### 1. Extract executable-only logic from `main()`

The current `main()` in [src/bhyverun.c](scorpi/src/bhyverun.c) should be split into library functions such as:

- `scorpi_runtime_prepare(spec, ...)`
- `scorpi_runtime_create_vm(...)`
- `scorpi_runtime_start(...)`
- `scorpi_runtime_run_event_loop(...)`
- `scorpi_runtime_destroy(...)`

`main()` should become a thin wrapper around those calls.

### 2. Separate input parsing from runtime launch

Today, CLI parsing and launch are tightly coupled. Move parsing into:

- CLI parser module
- YAML parser module
- typed builder API

All three should produce the same normalized internal VM definition.

### 3. Replace process-global config initialization at the API boundary

Current code uses `init_config()` once and stores config globally. That is acceptable for the current executable but weak for a library.

Short-term:

- create/reset config as part of VM creation

Long-term:

- make config state owned by a VM runtime object, not a global singleton

### 4. Stabilize device naming and descriptors

The library needs a documented set of device identifiers and discoverable property names. Existing strings such as:

- `virtio-net`
- `virtio-blk`
- `xhci`
- `ahci`
- `hostbridge`

should become part of the supported public contract, alongside runtime-discoverable property metadata for each device.

### 5. Add structured validation before launch

Validation should catch:

- duplicate PCI or USB locations
- missing required device properties
- invalid CPU core count
- unsupported backend/device combinations
- conflicting firmware settings
- malformed MAC addresses and paths
- conflicting or missing VM-control channel configuration when required
- missing USB parent controller when more than one eligible controller exists
- invalid or conflicting generic parent references in device hierarchies

Today many of these errors are only discovered during initialization.

## `scorpi` Executable After Refactor

The `scorpi` binary should do only this:

1. Parse arguments.
2. Build or load a `scorpi_vm_t`.
3. Optionally print validation or normalized config.
4. Launch the VM via `scorpi_kit`.
5. Exit with a meaningful status code.

Possible CLI shape:

```text
scorpi -f vm.yaml
```

Backward compatibility with existing flags can be preserved temporarily by translating legacy CLI options into the new spec builder.

This executable is intentionally narrower than the future Scorpi product. It is a direct local consumer of `scorpi_kit`, not the system that owns fleet state.

## Future Product Boundary

The future Scorpi product should be treated as a separate layer built on top of `scorpi_kit`.

Expected shape:

- a daemon process manages VM inventory and long-lived state
- Docker-like APIs are exposed by that daemon
- client commands call the daemon instead of linking runtime logic directly
- viewer commands attach to sockets or channels exposed by running VMs

The VM-control socket fits this boundary well:

- local `scorpi` can use it directly in the current phase
- `ScorpiViewer` can use it for keyboard and mouse injection
- future clients or the daemon can use it for guest control actions such as shutdown

Likely future commands:

- `scorpi run`
- `scorpi ps`
- `scorpi stop`
- `scorpi snapshot`
- `scorpi view`

For graphics, `scorpi view` should be treated as a client concern. The VM runtime should expose a stable connection point for viewers, while the actual windowing implementation stays outside `scorpi_kit`. The existing `ScorpiViewer` project is consistent with this separation.

More generally, the runtime should expose a stable control endpoint for VM-control commands. Viewer input injection and VM control requests should be seen as two uses of the same control channel.

## Meson Build Changes

The build should produce both:

- `libscorpi_kit`
- `scorpi`

Suggested direction:

- create an internal static library for shared runtime/device objects
- create a public library target for `scorpi_kit`
- link the CLI executable against `scorpi_kit`

This avoids compiling the same runtime sources twice and makes the layering visible in the build graph.

## Error Handling

The library needs predictable error reporting.

Requirements:

- functions return explicit status codes
- an error object carries message, subsystem, and optional source location
- YAML errors should include line and column
- validation errors should reference the spec path, for example `devices.pci[2].path`

Do not rely only on `err()`, `errx()`, or direct process termination in public library paths. Those are acceptable in the current executable but not in an embeddable library.

## Threading and Lifecycle Considerations

The current runtime uses global state and enters `mevent_dispatch()`. For the library API, the following behavior should be explicit:

- whether `scorpi_kit_vm_start()` blocks
- whether the event loop runs on the caller thread or an internal thread
- how shutdown is requested
- whether multiple VMs in one process are supported
- how the VM-control channel is created, advertised, and torn down

Recommended first version:

- support one active VM per process
- run the event loop on the caller thread
- expose `start`, `wait`, `stop`, and `destroy`

This keeps the first library version simple and consistent with the current code.

## Migration Plan

### Milestone 1: document and codify the VM spec

- define the public `scorpi_vm_t` builder model
- define public device identifiers
- add validation

### Milestone 2: create the config adapter

- generate current config nodes from the typed spec
- prove feature parity with existing CLI behavior

### Milestone 3: move launch logic into library

- extract non-CLI startup sequence from `main()`
- create `libscorpi_kit`
- convert `scorpi` to call the library

### Milestone 4: add YAML support

- parse YAML into the typed spec
- add `validate` and `run --file` CLI flows

### Milestone 5: reduce global state

- make config and runtime state VM-owned
- remove assumptions that only CLI code drives startup

### Milestone 6: incrementally type device config

- migrate commonly used devices first
- keep adapter fallback for remaining devices

Linux/KVM support and the daemon product are intentionally outside these milestones. The current milestones should establish the library and layering needed for those later efforts.

## Testing Strategy

Add tests at three levels:

- spec parsing tests
  - YAML to spec
  - API builder to spec
- validation tests
  - invalid CPU core count
  - duplicate device slots
  - required fields
- adapter parity tests
  - legacy CLI-generated config vs API/YAML-generated config

The most valuable regression test is parity: the same VM definition expressed through CLI, API, and YAML should normalize to the same internal spec and equivalent runtime config.

## Risks

- The current use of global config and process-wide state makes true library embedding harder than the API split alone suggests.
- Some initialization paths still terminate the process directly instead of returning errors.
- The event-loop ownership model is currently executable-oriented.
- An open-ended property model needs strong runtime discovery and validation rules, otherwise property support and error messages will become inconsistent.

## Missing Product-Level Concerns

The document should also leave room for the following future concerns, even though they are not part of the current implementation phase:

- hypervisor backend abstraction so the library can support more than the current backend later
- persistent VM metadata and naming rules
- image and disk inventory management
- snapshot metadata, retention, and restore semantics
- structured VM stats and event streams
- API versioning and compatibility rules
- authentication and authorization for daemon APIs
- crash recovery and reconciliation after daemon restart
- logging, audit trail, and diagnostics collection
- viewer/session authorization and console attach policy

## Recommendation

Implement `scorpi_kit` as a typed spec plus launch library, with a temporary adapter into the existing config tree. Build `scorpi` as a thin local executable over that library, but keep the future Docker-like Scorpi product explicitly separate as a later daemon/client layer.

The main rule for the refactor should be:

> YAML, the local CLI, future daemon APIs, and direct library calls must all produce the same typed VM spec, and only that spec may drive VM creation.
