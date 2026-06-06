# Linux ARM64 KVM Port Task List

This document tracks the first Linux porting milestone: run Scorpi on ARM64
Linux with KVM as the hypervisor engine instead of Apple HVF.

The main refactoring rule is to keep the device model and ARM64 platform code
behind the existing `vmmapi.h` interface. HVF and KVM should be backend
implementations, not separate copies of the VM runtime.

Current status, 2026-05-15:

- Linux ARM64 configure, build, and unit tests work in the Alma guest with KVM
  selected.
- `build.sh` builds Linux ARM64 into `build-linux-arm64` and macOS ARM64 into
  `build-macos-arm64`.
- The Linux `mevent` backend uses `epoll`, `eventfd`, `timerfd`, `inotify`,
  and `signalfd` for `EVF_SIGNAL`; it does not install signal handlers.
- KVM ARM64 now has VM/vCPU creation, memory registration, register access,
  VGICv3, IRQ injection, MSI injection, and a `KVM_RUN` loop.
- FreeBSD ARM64 boots under KVM with U-Boot/EFI, 4 vCPUs come up through
  in-kernel PSCI, and in-process guest reboot works for the current
  virtio-block/net boot path.
- The current in-process reboot model is intentional for now. It differs from
  upstream bhyve's default process-restart model, so reset participants must be
  explicit.
- Linux/x86 KVM is deferred. Keep interfaces portable, but do not add x86
  backend implementation in this milestone.

## Milestone 0: Linux Build Baseline

- [x] Verify the Linux guest path used for development:
  `scorpi run alma -- sh -lc 'cd <scorpi-hv-checkout> && ...'`.
- [x] Install or document Linux build dependencies:
  - `meson`
  - `ninja`
  - `cmake`
  - C compiler
  - `pkg-config`
  - `glib2-devel` or distro equivalent
  - zlib development package
  - `dtc`
  - `libuuid-devel` or distro equivalent
- [x] Fix the current Meson setup failure on Linux caused by missing
  `glib-2.0` for the `libslirp` subproject.
- [x] Make Meson setup work on ARM64 Linux.
- [x] Make non-runtime tests compile on Linux where possible.
- [x] Replace the temporary Linux `mevent` poll/signal implementation with
  fd-native Linux primitives.
- [ ] Add a short Linux/KVM build and run section to `BUILD.md`.

Acceptance check:

```sh
scorpi run alma -- 'cd scorpi-hv && ./build.sh'
scorpi run alma -- 'cd scorpi-hv && meson test -C build-linux-arm64 --print-errorlogs'
```

## Milestone 1: Explicit Hypervisor Backend Selection

- [x] Add a Meson option:
  `-Dhypervisor=auto|hvf|kvm`.
- [x] Select `hvf` automatically for `darwin/aarch64`.
- [x] Select `kvm` automatically for `linux/aarch64`.
- [x] Fail with a clear error for unsupported host/backend combinations.
- [x] Move backend source selection out of the current Darwin-only block.
- [x] Keep `src/arch/arm64/*.c` selected by CPU architecture, not by backend.

Expected source layout:

```text
src/libvmm/common/
src/libvmm/hvf/
src/libvmm/kvm/
src/libvmm/kvm/arm64/
```

Future Linux/x86 KVM can add `src/libvmm/kvm/x86/` when that port starts.

## Milestone 2: Clean Backend Boundaries

- [ ] Move backend-private `struct vmctx` and `struct vcpu` definitions out of
  public ARM64 headers.
- [ ] Keep `struct vmctx` and `struct vcpu` opaque to code that includes
  `include/vmmapi.h`.
- [ ] Create backend-private headers:
  - `src/libvmm/hvf/internal.h`
  - [x] `src/libvmm/kvm/internal.h`
- [ ] Add a Scorpi-private helper header for non-public runtime helpers, for
  example `include/vmmapi_internal.h`.
- [ ] Move these helper declarations out of `vmmapi.h` if they are not part of
  the intended public API:
  - `vm_setup_bootrom_segment`
  - `vm_setup_memory_segment`
  - `vm_vcpu_init`
  - `vcpu_reset`
  - `vm_get_spi_interrupt_range`
  - CPU active/suspend helper APIs if they are runtime-private
- [ ] Update includes in platform/device code to use the private helper header
  only where needed.

Acceptance check:

- Device files should not include backend-specific headers.
- ARM64 platform code should not mention HVF or KVM types.

## Milestone 3: Extract Backend-Neutral Code

- [ ] Split `src/libvmm/hvf/vmmapi.c` into backend-specific and shared parts.
- [x] Move instruction emulation to `src/libvmm/common/`.
- [ ] Move guest RAM range management to `src/libvmm/common/memory.c`.
- [ ] Move lowmem/highmem helpers to common code:
  - `vm_setup_memory`
  - `vm_map_gpa`
  - `vm_get_lowmem_size`
  - `vm_get_highmem_base`
  - `vm_get_highmem_size`
- [ ] Move CPU active/suspended mask helpers to common code if KVM can share
  them.
- [ ] Move copy helpers to common code:
  - `vm_copyin`
  - `vm_copyout`
  - `vm_copy_teardown`
- [ ] Keep backend callbacks for the operations that differ:
  - map guest memory into the hypervisor
  - create/destroy VM
  - create/destroy vCPU
  - run vCPU
  - get/set registers
  - IRQ injection

Acceptance check:

- HVF behavior should be unchanged after the split.
- KVM files can reuse memory bookkeeping without including HVF headers.

## Milestone 4: KVM Backend Skeleton

- [x] Add `src/libvmm/kvm/internal.h`.
- [x] Add `src/libvmm/kvm/vmmapi.c`.
- [ ] Split ARM64 VGIC code out of `src/libvmm/kvm/arm64/kvm_arch.c` into
  `src/libvmm/kvm/arm64/vgic.c` if it grows.
- [x] Open `/dev/kvm` in `vm_openf`.
- [x] Check `KVM_GET_API_VERSION == KVM_API_VERSION`.
- [x] Create the VM with `KVM_CREATE_VM`.
- [ ] For ARM64, query IPA size support with `KVM_CHECK_EXTENSION`.
- [x] Store KVM fds in the KVM `struct vmctx`:
  - `/dev/kvm` fd
  - VM fd
  - VGIC device fd
- [x] Return clear errors instead of `exit(-1)` on the implemented KVM paths.

Acceptance check:

- A minimal program path can create and close a KVM VM on Linux.

## Milestone 5: KVM Guest Memory

- [x] Implement KVM mapping for `vm_setup_memory_segment`.
- [x] Use page-aligned host memory allocations.
- [x] Register guest memory with `KVM_SET_USER_MEMORY_REGION`.
- [x] Track KVM memory slot IDs in the memory range table.
- [x] Preserve the existing ARM64 memory layout:
  - no lowmem on ARM64
  - highmem base at `0x100000000`
- [x] Support bootrom mappings.
- [ ] Verify framebuffer/virtio-gpu mappings under KVM.
- [ ] Decide how to represent read-only guest memory:
  KVM memory regions are userspace mappings, so protection may need host-side
  `mprotect` or a documented MVP limitation.

Acceptance check:

- `vm_map_gpa` returns valid host pointers for guest RAM and bootrom ranges.
- KVM accepts all registered guest memory regions.

## Milestone 6: KVM vCPU Lifecycle and Registers

- [x] Implement `vm_vcpu_open`.
- [x] Implement `vm_vcpu_init` with:
  - [x] `KVM_CREATE_VCPU`
  - [x] `KVM_GET_VCPU_MMAP_SIZE`
  - [x] mmap of `struct kvm_run`
  - [x] `KVM_ARM_PREFERRED_TARGET`
  - [x] `KVM_ARM_VCPU_INIT`
- [x] Implement `vm_vcpu_deinit` and `vm_vcpu_close`.
- [x] Implement `vcpu_id`.
- [x] Implement `vm_set_register` and `vm_get_register` through
  `KVM_SET_ONE_REG` and `KVM_GET_ONE_REG`.
- [ ] Map Scorpi registers to KVM ARM64 register IDs:
  - [x] X0-X30
  - [x] SP
  - [x] PC
  - [x] PSTATE/CPSR
- [ ] Add additional ARM64 system registers only when a concrete KVM user
  appears, such as save/restore, debug state, or instruction emulation that
  needs guest translation state.
- [ ] Implement deferred cross-thread register writes or document that KVM MVP
  only supports direct register access for created vCPUs.
- [x] Initialize BSP state to match the HVF boot path.
- [x] Enable ARM64 in-kernel PSCI 0.2 for KVM vCPU bring-up.
- [x] Reset KVM vCPU state for in-process reboot.

Acceptance check:

- Scorpi can create the requested vCPU count and set the boot PC.

## Milestone 7: KVM VGICv3 and Interrupts

- [x] Create a VGICv3 device with `KVM_CREATE_DEVICE` and
  `KVM_DEV_TYPE_ARM_VGIC_V3`.
- [x] Error out if `KVM_DEV_TYPE_ARM_VGIC_V3` is not supported.
- [x] Set distributor base with `KVM_VGIC_V3_ADDR_TYPE_DIST`.
- [x] Set redistributor base or redistributor region.
- [x] Set the number of IRQs with `KVM_DEV_ARM_VGIC_GRP_NR_IRQS`.
- [x] Initialize the VGIC with `KVM_DEV_ARM_VGIC_CTRL_INIT` after vCPU
  creation.
- [x] Implement `vm_assert_irq` and `vm_deassert_irq` with `KVM_IRQ_LINE`.
- [x] Implement a conservative `vm_get_spi_interrupt_range`.
- [x] Implement `vm_raise_msi` through GICv3 MBI doorbell semantics.
- [ ] Defer irqfd/eventfd MSI acceleration until after first boot.

Acceptance check:

- PL011 and RTC interrupts can be asserted and deasserted through KVM.

## Milestone 8: KVM_RUN Exit Handling

- [x] Implement `vm_run` around `ioctl(vcpu_fd, KVM_RUN, 0)`.
- [x] Convert `KVM_EXIT_MMIO` to existing Scorpi MMIO handling.
- [x] Convert ARM64 PSCI/system events:
  - [x] `KVM_EXIT_SYSTEM_EVENT` shutdown -> `VM_SUSPEND_POWEROFF`
  - [x] `KVM_EXIT_SYSTEM_EVENT` reset -> `VM_SUSPEND_RESET`
  - [x] crash -> `VM_SUSPEND_HALT`
- [x] Handle `KVM_EXIT_FAIL_ENTRY` and `KVM_EXIT_INTERNAL_ERROR` with useful
  diagnostics.
- [x] Decide whether KVM MMIO exits should use `VM_EXITCODE_INST_EMUL` or call
  `emulate_mem` directly: KVM currently calls `read_mem`/`write_mem` directly.
- [x] Verify that HVC/SMC PSCI startup behavior matches the existing ARM64
  `vmexit_smccc` expectations.

Acceptance check:

- A guest reaches the first MMIO console access and Scorpi services it.

## Milestone 9: First Boot Target

- [ ] Create a minimal Linux ARM64 YAML sample for KVM.
- [x] Start with one vCPU.
- [x] Enable only the minimum devices:
  - hostbridge
  - bootrom
  - PL011 UART
  - RTC
  - fwcfg/FDT path needed by the current ARM64 platform
- [x] Boot to UEFI/U-Boot output.
- [x] Boot FreeBSD ARM64 to installer/login over serial.
- [x] Save a known-good command:

```sh
scorpi run alma -- 'cd scorpi-hv && ./build-linux-arm64/scorpi-hv -f ./samples/freebsd_vm.yaml'
```

Acceptance check:

- Serial output appears from the guest on Linux/KVM.

## Milestone 10: Device Bring-Up After First Boot

- [x] Bring up virtio block.
- [x] Bring up virtio net with slirp or socket backend.
- [x] Bring up PCI IRQ routing.
- [x] Bring up MSI through GICv3 MBI.
- [ ] Bring up framebuffer or virtio-gpu.
- [ ] Bring up USB only after PCI and interrupts are stable.
- [ ] Bring up TPM only after core boot paths are stable.
- [ ] Revisit Windows ARM64 only after Linux/FreeBSD boot paths are reliable.
- [x] Support in-process guest reboot for the current virtio block/net boot
  path.
- [ ] Audit and add reset hooks for non-virtio devices used under KVM:
  - AHCI
  - e1000
  - XHCI
  - framebuffer/virtio-gpu display state
  - platform devices

## Milestone 11: Cleanup and Tests

- [ ] Add unit tests for backend-neutral memory slot bookkeeping.
- [x] Add compile tests for Linux-only headers.
- [ ] Add a CI-friendly Meson setup path that does not require `/dev/kvm`.
- [x] Add a runtime smoke test gated on `/dev/kvm` availability.
- [ ] Add a repeatable FreeBSD ARM64 KVM reboot smoke script/test that is not
  part of the default unit suite.
- [ ] Document required host kernel capabilities.
- [ ] Document known limitations:
  - nested virtualization
  - MSI acceleration
  - snapshots
  - cross-thread register access
  - Windows support status

## Current Known Blockers

- `struct vmctx` is public in `include/arch/arm64/vmm.h`, which makes a clean
  second backend awkward.
- Several Scorpi-private helpers are declared in public `vmmapi.h`.
- Linux/x86 KVM remains deferred.
- KVM does not yet query/validate ARM64 IPA size.
- KVM register mapping intentionally covers only the registers needed by the
  current run/reset path; additional ARM64 system registers should be added
  when a concrete KVM feature needs them.
- KVM read-only memory behavior needs an explicit decision and test.
- In-process reset is implemented for the current FreeBSD virtio boot path, but
  non-virtio device reset coverage is incomplete.
- No committed automated reboot smoke test exists yet.

## Useful References

- KVM API documentation:
  <https://docs.kernel.org/virt/kvm/api.html>
- KVM ARM64 vCPU feature selection:
  <https://docs.kernel.org/virt/kvm/arm/vcpu-features.html>
- KVM ARM VGICv3 device:
  <https://www.kernel.org/doc/html/latest/virt/kvm/devices/arm-vgic-v3.html>
