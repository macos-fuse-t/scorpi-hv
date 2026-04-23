# Scorpi Kit Implementation Plan

## Purpose

This document turns the `scorpi_kit` design into an execution plan.

Each task has:

- scope
- dependencies
- implementation notes
- validation criteria

A task is not complete until its validation criteria pass.

## Execution Rules

- Keep each task small enough to validate independently.
- Do not move to the next task until the current task passes validation.
- Prefer additive changes that preserve the current executable until the library path is proven.
- Validation must be runnable from the repo, not just asserted in prose.

## Validation Levels

- Build validation
  - Meson reconfigure succeeds.
  - Required targets compile.
- Unit validation
  - New tests compile and pass.
- Behavioral validation
  - New API behavior matches the design.
- Parity validation
  - API-built and YAML-built VMs normalize to the same internal representation.

## Task List

### Task 0: Vendor YAML Parser

Status:

- Done

Scope:

- vendor `libyaml` under `libyaml/`
- add local Meson target
- keep it local only, not yet used by runtime code

Dependencies:

- none

Validation criteria:

- `meson setup builddir --reconfigure` succeeds
- `meson compile -C builddir yaml` succeeds
- vendored tree contains only:
  - `include/`
  - `src/`
  - `LICENSE`
  - local `meson.build`

### Task 1: Create Public Library Build Target

Scope:

- create a real `libscorpi_kit` target
- wire headers and sources for the new library
- keep `scorpi` building unchanged

Dependencies:

- Task 0

Implementation notes:

- add a separate Meson target for the library
- avoid linking runtime launch logic yet
- start with builder/data-model sources only

Validation criteria:

- `meson setup builddir --reconfigure` succeeds
- `meson compile -C builddir` succeeds
- Meson produces a `scorpi_kit` library target
- existing `scorpi` target still compiles

### Task 2: Replace Placeholder Public Header

Scope:

- replace the placeholder API in `include/scorpi.h`
- define the `scorpi_*` API shape from the design
- expose opaque types only

Dependencies:

- Task 1

Implementation notes:

- add forward declarations for:
  - `scorpi_vm_t`
  - `scorpi_device_t`
  - `scorpi_error_t`
- define the public function prototypes only
- do not expose internal structs

Validation criteria:

- library compiles with the new public header
- a small C compile test can include `scorpi.h` and compile
- placeholder functions and old placeholder signatures are removed

Suggested validation:

- add a compile-only test source under `tests/` that includes `scorpi.h`

### Task 3: Implement Error Type and Error Contract

Scope:

- implement `scorpi_error_t`
- make fallible library functions return `scorpi_error_t`
- define success and failure semantics consistently

Dependencies:

- Task 2

Implementation notes:

- decide whether `scorpi_error_t` is an enum, struct, or enum+details pair
- provide at least:
  - success value
  - invalid argument
  - duplicate id
  - invalid parent
  - unsupported device
  - YAML parse error
  - validation failure
  - runtime failure

Validation criteria:

- all currently implemented fallible public APIs return `scorpi_error_t`
- unit tests cover at least 3 failing paths and 1 success path
- no fallible public API returns `int`

### Task 4: Implement VM Builder Object

Scope:

- implement `scorpi_create_vm`
- implement VM lifetime management
- implement VM property storage
- implement `scorpi_vm_set_cpu` and `scorpi_vm_set_ram`

Dependencies:

- Task 3

Implementation notes:

- VM should store:
  - scalar properties
  - device list
  - ids index
- generic VM property setters should support:
  - string
  - bool
  - u64
- `scorpi_vm_set_cpu` and `scorpi_vm_set_ram` should normalize into the same VM property space used by generic setters

Validation criteria:

- unit tests verify:
  - `scorpi_create_vm` succeeds
  - `scorpi_vm_set_cpu(vm, 2)` stores a normalized CPU property
  - `scorpi_vm_set_ram(vm, bytes)` stores a normalized memory property
  - `scorpi_destroy_vm` is safe on a populated VM

Suggested validation:

- add tests for `cpu.cores`
- add tests for numeric memory storage

### Task 5: Implement Device Builder Objects

Scope:

- implement:
  - `scorpi_create_pci_device`
  - `scorpi_create_usb_device`
  - `scorpi_create_lpc_device`
- implement device property storage
- implement optional PCI slot-at-creation behavior

Dependencies:

- Task 4

Implementation notes:

- store the bus kind in the device object
- store optional PCI slot during creation
- `SCORPI_PCI_SLOT_AUTO` should mean “unset”

Validation criteria:

- unit tests verify:
  - creating PCI, USB, and LPC devices succeeds
  - PCI slot is stored when explicitly provided
  - PCI slot is unset when `SCORPI_PCI_SLOT_AUTO` is used
  - generic device properties can be set and retrieved internally

### Task 6: Implement `scorpi_vm_add_device`

Scope:

- attach device objects to a VM
- transfer ownership correctly
- track ids

Dependencies:

- Task 5

Implementation notes:

- `scorpi_vm_add_device` should reject:
  - null device
  - duplicate `id`
  - invalid bus/device combinations if already known

Validation criteria:

- unit tests verify:
  - adding a device succeeds
  - duplicate device ids fail
  - ownership is transferred correctly
- adding a device twice fails

### Task 7: Implement Parent Reference Resolution

Scope:

- support `parent` as a generic device property
- resolve parent references by `id`
- support future non-USB hierarchies

Dependencies:

- Task 6

Implementation notes:

- `parent` remains a generic property in the public API
- validation layer resolves it semantically

Validation criteria:

- unit tests verify:
  - child can reference an existing parent id
  - missing parent id fails
  - self-parenting fails
  - generic hierarchy works for a non-USB example such as `pci-bridge`

### Task 8: Implement Internal Normalized VM Representation

Scope:

- convert builder objects into a normalized internal VM representation
- ensure all input paths converge on the same form

Dependencies:

- Task 7

Implementation notes:

- normalize:
  - shorthand CPU/RAM semantics
  - optional PCI slot representation
  - parent references
  - device property maps

Validation criteria:

- unit tests verify normalization output is deterministic
- repeated normalization of the same VM yields identical results
- normalized form contains no unresolved shorthand

### Task 9: Implement YAML Loader Skeleton

Scope:

- implement `scorpi_load_vm_from_yaml`
- parse YAML into an intermediate structure
- map it into the VM builder model

Dependencies:

- Task 8

Implementation notes:

- use vendored `libyaml`
- do not bind directly to runtime code
- preserve unknown but syntactically valid device properties

Validation criteria:

- unit tests verify:
  - valid YAML loads successfully
  - malformed YAML returns YAML parse error
  - line/column are captured for syntax failures if the error type supports them

### Task 10: Implement YAML Normalization Rules

Scope:

- support:
  - `cpu: 2`
  - `memory: 4G`
  - expanded forms like `cpu: { cores: 2 }`
  - expanded forms like `memory: { size: 4G }`

Dependencies:

- Task 9

Implementation notes:

- shorthand and expanded forms must create the same normalized internal VM representation

Validation criteria:

- tests compare normalized output for:
  - shorthand CPU vs expanded CPU
  - shorthand memory vs expanded memory
- results must be byte-for-byte equal if serialized or structurally equal if compared in memory

### Task 11: Implement Semantic Validation

Scope:

- validate VM and device semantics before launch

Dependencies:

- Task 10

Implementation notes:

- validate at least:
  - missing required properties
  - invalid CPU core count
  - invalid RAM size
  - duplicate ids
  - invalid parent references
  - duplicate PCI slots
  - missing USB parent when required
  - invalid LPC device config for `vm-control` and `tpm`

Validation criteria:

- tests cover every bullet above
- each invalid case returns the expected validation error
- one complete valid configuration passes

### Task 12: Implement Builder-to-Config Adapter

Scope:

- convert the normalized VM representation into the current config tree

Dependencies:

- Task 11

Implementation notes:

- populate existing config paths used by:
  - PCI init
  - USB init
  - TPM init
  - VM-control socket startup
- keep current device code unchanged as much as possible

Validation criteria:

- adapter tests verify expected config nodes are created for:
  - CPU
  - memory
  - PCI devices
  - USB devices
  - `vm-control`
  - TPM
- a golden config dump or structural comparison passes

### Task 13: Extract Launch Logic from `main()`

Scope:

- move launch/runtime setup out of `main()`
- make it callable from the library

Dependencies:

- Task 12

Implementation notes:

- split `main()` into runtime entrypoints
- preserve existing `scorpi` behavior while refactoring

Validation criteria:

- `scorpi` still launches through the legacy CLI path
- library-side launch entrypoint compiles
- no behavior regression in basic startup path

### Task 14: Implement `scorpi_start_vm`, `scorpi_wait_vm`, `scorpi_stop_vm`

Scope:

- wire public lifecycle APIs to extracted runtime logic

Dependencies:

- Task 13

Implementation notes:

- `scorpi_start_vm()` performs final validation before launch
- `scorpi_wait_vm()` and `scorpi_stop_vm()` should be consistent with the current event-loop ownership model

Validation criteria:

- library integration test can:
  - create a VM
  - add minimal devices
  - start it
  - stop it or observe expected startup failure cleanly
- lifecycle APIs return `scorpi_error_t`

### Task 15: Convert `scorpi` CLI to Use `scorpi_kit`

Scope:

- make the local executable use the library path

Dependencies:

- Task 14

Implementation notes:

- preserve current CLI behavior where practical
- translate CLI options into the same builder model used by YAML

Validation criteria:

- existing basic CLI launch still works
- CLI-generated VM definition and API-generated VM definition normalize equivalently for at least one shared scenario

### Task 16: Run `scorpi` from a YAML File

Scope:

- add CLI support for `-f cfg.yaml`
- load the YAML file through `scorpi_load_vm_from_yaml`
- launch the resulting VM through the same `scorpi_kit` lifecycle path as API-built VMs

Dependencies:

- Task 15

Implementation notes:

- `-f` should be a local CLI input mode for the thin `scorpi` executable
- file loading should remain outside the core library API surface
- YAML-file launch should share the same normalized builder path as:
  - direct API construction
  - future daemon-driven creation
- decide and document precedence if `-f` is combined with legacy config-producing CLI flags

Validation criteria:

- `scorpi -f cfg.yaml` parses a valid YAML file and reaches the library launch path
- invalid YAML file content returns a YAML parse failure with a non-zero exit code
- missing YAML file returns a non-zero exit code
- a parity test verifies that:
  - `scorpi_load_vm_from_yaml(file_contents)` and
  - loading the same YAML through the `-f` CLI path
  normalize equivalently for at least one scenario

### Task 17: Add Parity Tests Between YAML and API

Scope:

- ensure YAML and API are two frontends for the same model

Dependencies:

- Task 16

Implementation notes:

- use at least one realistic VM shape:
  - hostbridge
  - xhci
  - block device
  - virtio-net
  - `vm-control`
  - TPM

Validation criteria:

- API-built VM and YAML-built VM normalize to the same internal representation
- parity test runs in CI/local test target

### Task 18: Add Documentation and Usage Examples

Scope:

- document public API and YAML examples
- update README/BUILD docs if needed

Dependencies:

- Task 17

Validation criteria:

- documentation references the actual public API names
- at least one API example compiles or is compile-checked
- at least one YAML example is covered by a loader test

## Task Completion Checklist

Use this checklist for every task:

- code implemented
- tests added or updated
- validation criteria run locally
- validation passed
- no unrelated files changed

## Recommended Immediate Next Task

Task 1: Create Public Library Build Target

Reason:

- `libyaml` is already vendored
- current `include/scorpi.h` and `src/libscorpi/scorpi.c` are only placeholders
- this is the smallest step that enables real implementation work to start behind a library boundary
