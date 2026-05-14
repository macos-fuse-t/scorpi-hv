# Linux ARM64 KVM Port Task List

This document tracks the first Linux porting milestone: run Scorpi on ARM64
Linux with KVM as the hypervisor engine instead of Apple HVF.

The main refactoring rule is to keep the device model and ARM64 platform code
behind the existing `vmmapi.h` interface. HVF and KVM should be backend
implementations, not separate copies of the VM runtime.

Current status, 2026-05-14:

- Linux ARM64 configure and build works in the Alma guest with KVM selected.
- Use a guest-local build directory such as `/tmp/scorpi-build-linux`; building
  in the shared mount produced depfile I/O errors.
- The Linux `mevent` backend uses `epoll`, `eventfd`, `timerfd`, `inotify`,
  and `signalfd` for `EVF_SIGNAL`; it does not install signal handlers.
- KVM is a compile-time backend skeleton only. It is not expected to boot a
  guest until the KVM fd/ioctl, memory, vCPU, VGIC, and run-loop tasks below are
  implemented.
- Linux/x86 KVM is deferred. Keep interfaces portable, but do not add x86
  backend implementation in this milestone.

## Milestone 0: Linux Build Baseline

- [x] Verify the Linux guest path used for development:
  `scorpi run alma -- sh -lc 'cd /home/alexf/scorpi-hv && ...'`.
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
- [ ] Add a short Linux build section to `BUILD.md`.

Acceptance check:

```sh
scorpi run alma -- /usr/bin/meson setup /tmp/scorpi-build-linux /home/alexf/scorpi-hv
scorpi run alma -- /usr/bin/ninja -C /tmp/scorpi-build-linux
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
- [ ] Add `src/libvmm/kvm/vgic.c`.
- [ ] Open `/dev/kvm` in `vm_openf`.
- [ ] Check `KVM_GET_API_VERSION == KVM_API_VERSION`.
- [ ] Create the VM with `KVM_CREATE_VM`.
- [ ] For ARM64, query IPA size support with `KVM_CHECK_EXTENSION`.
- [ ] Store KVM fds in the KVM `struct vmctx`:
  - `/dev/kvm` fd
  - VM fd
  - VGIC device fd
- [ ] Return clear errors instead of `exit(-1)` where practical.

Acceptance check:

- A minimal program path can create and close a KVM VM on Linux.

## Milestone 5: KVM Guest Memory

- [ ] Implement KVM mapping for `vm_setup_memory_segment`.
- [ ] Use page-aligned host memory allocations.
- [ ] Register guest memory with `KVM_SET_USER_MEMORY_REGION`.
- [ ] Track KVM memory slot IDs in the memory range table.
- [ ] Preserve the existing ARM64 memory layout:
  - no lowmem on ARM64
  - highmem base at `0x100000000`
- [ ] Support bootrom and framebuffer mappings.
- [ ] Decide how to represent read-only guest memory:
  KVM memory regions are userspace mappings, so protection may need host-side
  `mprotect` or a documented MVP limitation.

Acceptance check:

- `vm_map_gpa` returns valid host pointers for guest RAM and bootrom ranges.
- KVM accepts all registered guest memory regions.

## Milestone 6: KVM vCPU Lifecycle and Registers

- [ ] Implement `vm_vcpu_open`.
- [ ] Implement `vm_vcpu_init` with:
  - `KVM_CREATE_VCPU`
  - `KVM_GET_VCPU_MMAP_SIZE`
  - mmap of `struct kvm_run`
  - `KVM_ARM_PREFERRED_TARGET`
  - `KVM_ARM_VCPU_INIT`
- [ ] Implement `vm_vcpu_deinit` and `vm_vcpu_close`.
- [ ] Implement `vcpu_id`.
- [ ] Implement `vm_set_register` and `vm_get_register` through
  `KVM_SET_ONE_REG` and `KVM_GET_ONE_REG`.
- [ ] Map Scorpi registers to KVM ARM64 register IDs:
  - X0-X30
  - SP
  - PC
  - PSTATE/CPSR
  - EL1 system registers used by paging/emulation
- [ ] Implement deferred cross-thread register writes, or document that early
  KVM MVP only supports local-thread register access.
- [ ] Initialize BSP state to match the HVF boot path.

Acceptance check:

- Scorpi can create the requested vCPU count and set the boot PC.

## Milestone 7: KVM VGICv3 and Interrupts

- [ ] Create a VGICv3 device with `KVM_CREATE_DEVICE` and
  `KVM_DEV_TYPE_ARM_VGIC_V3`.
- [ ] Set distributor base with `KVM_VGIC_V3_ADDR_TYPE_DIST`.
- [ ] Set redistributor base or redistributor region.
- [ ] Set the number of IRQs with `KVM_DEV_ARM_VGIC_GRP_NR_IRQS`.
- [ ] Initialize the VGIC with `KVM_DEV_ARM_VGIC_CTRL_INIT` after vCPU
  creation.
- [ ] Implement `vm_assert_irq` and `vm_deassert_irq` with `KVM_IRQ_LINE`.
- [ ] Implement a conservative `vm_get_spi_interrupt_range`.
- [ ] Implement or stub `vm_raise_msi`.
- [ ] Defer irqfd/eventfd MSI acceleration until after first boot.

Acceptance check:

- PL011 and RTC interrupts can be asserted and deasserted through KVM.

## Milestone 8: KVM_RUN Exit Handling

- [ ] Implement `vm_run` around `ioctl(vcpu_fd, KVM_RUN, 0)`.
- [ ] Convert `KVM_EXIT_MMIO` to existing Scorpi MMIO handling.
- [ ] Convert ARM64 PSCI/system events:
  - `KVM_EXIT_SYSTEM_EVENT` shutdown -> `VM_SUSPEND_POWEROFF`
  - `KVM_EXIT_SYSTEM_EVENT` reset -> `VM_SUSPEND_RESET`
  - suspend where supported -> `VM_SUSPEND_HALT` or a new mapping
- [ ] Handle `KVM_EXIT_FAIL_ENTRY` and `KVM_EXIT_INTERNAL_ERROR` with useful
  diagnostics.
- [ ] Decide whether KVM MMIO exits should use `VM_EXITCODE_INST_EMUL` or call
  `emulate_mem` directly.
- [ ] Verify that HVC/SMC PSCI startup behavior matches the existing ARM64
  `vmexit_smccc` expectations.

Acceptance check:

- A guest reaches the first MMIO console access and Scorpi services it.

## Milestone 9: First Boot Target

- [ ] Create a minimal Linux ARM64 YAML sample for KVM.
- [ ] Start with one vCPU.
- [ ] Enable only the minimum devices:
  - hostbridge
  - bootrom
  - PL011 UART
  - RTC
  - fwcfg/FDT path needed by the current ARM64 platform
- [ ] Boot to UEFI output or a simple kernel serial banner.
- [ ] Save a known-good command:

```sh
scorpi run alma -- sh -lc 'cd /home/alexf/scorpi-hv && ./build-linux/scorpi-hv ...'
```

Acceptance check:

- Serial output appears from the guest on Linux/KVM.

## Milestone 10: Device Bring-Up After First Boot

- [ ] Bring up virtio block.
- [ ] Bring up virtio net with slirp or socket backend.
- [ ] Bring up PCI IRQ routing.
- [ ] Bring up MSI or document INTx-only limitations.
- [ ] Bring up framebuffer or virtio-gpu.
- [ ] Bring up USB only after PCI and interrupts are stable.
- [ ] Bring up TPM only after core boot paths are stable.
- [ ] Revisit Windows ARM64 only after Linux/FreeBSD boot paths are reliable.

## Milestone 11: Cleanup and Tests

- [ ] Add unit tests for backend-neutral memory slot bookkeeping.
- [ ] Add compile tests for Linux-only headers.
- [ ] Add a CI-friendly Meson setup path that does not require `/dev/kvm`.
- [ ] Add a runtime smoke test gated on `/dev/kvm` availability.
- [ ] Document required host kernel capabilities.
- [ ] Document known limitations:
  - nested virtualization
  - MSI acceleration
  - snapshots
  - cross-thread register access
  - Windows support status

## Current Known Blockers

- Linux Meson setup currently fails before compiling because `glib-2.0` is
  missing for the `libslirp` subproject.
- `cmake` is missing on the current Alma guest, which will matter for the
  `libwebsockets` subproject.
- `struct vmctx` is public in `include/arch/arm64/vmm.h`, which makes a clean
  second backend awkward.
- Several Scorpi-private helpers are declared in public `vmmapi.h`.
- HVF-specific VGIC and MSI assumptions need a separate KVM implementation.

## Useful References

- KVM API documentation:
  <https://docs.kernel.org/virt/kvm/api.html>
- KVM ARM64 vCPU feature selection:
  <https://docs.kernel.org/virt/kvm/arm/vcpu-features.html>
- KVM ARM VGICv3 device:
  <https://www.kernel.org/doc/html/latest/virt/kvm/devices/arm-vgic-v3.html>
