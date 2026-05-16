# Scorpi x86/KVM Firmware-ACPI Platform Task Plan

## Summary

This document tracks the Linux x86/KVM Scorpi platform work.

The target is a modern UEFI x86 machine that boots Linux and Windows, uses
KVM in-kernel LAPIC/IOAPIC, avoids legacy chipset infrastructure, and builds
ACPI entirely in EDK2 firmware.

Controlling platform rule:

> No legacy chipset or legacy interrupt platform. Compatibility PCI/USB devices
> are allowed when they are independently enumerated through PCIe/USB and
> described by firmware ACPI.

Defaults:

- Linux profile: virtio-only by default for disk, network, console/GPU where
  available.
- Windows profile: no extra driver install; use AHCI, xHCI, USB HID
  keyboard/mouse, and inbox-compatible USB networking.
- No i440fx/Q35 chipset model.
- No guest-visible PIC, PIT, ISA, LPC, CMOS, CF8/CFC, or VMM-built ACPI tables.
- PCI discovery uses PCIe ECAM/MMCONFIG.
- Firmware owns ACPI generation from Scorpi hardware info.
- Fast-start is a later Linux-only track and must not complicate v1 UEFI boot.

## Architecture

- Add a Scorpi X64 EDK2 platform derived from modern OVMF/CloudHv-style
  initialization.
- Add a Scorpi hardware-info contract from `scorpi-hv` to firmware describing
  RAM, vCPU/APIC IDs, LAPIC/IOAPIC, GSI routing, PCIe ECAM, PCI MMIO windows,
  boot policy, display, TPM, and device profile.
- Build ACPI inside firmware from hardware info using DynamicTablesPkg. Do not
  use `etc/table-loader` or `etc/acpi/*`.
- Provide two default device profiles:
  - `linux-virtio`: virtio-blk or virtio-scsi, virtio-net,
    virtio-gpu/framebuffer, optional virtio-console/rng.
  - `windows-inbox`: AHCI, xHCI, USB HID, USB network, Scorpi GOP.
- Keep compatibility devices allowed, but do not introduce a legacy chipset or
  legacy interrupt platform.

## Scorpi X64 Hardware-Info ABI

The VMM exposes the Scorpi X64 hardware-info block as a qemu fw_cfg named file:

```text
opt/scorpi/x64-hardware-info
```

Firmware reads this file with `QemuFwCfgLib` during PEI/DXE, validates it, and
uses it as the single source of truth for memory HOBs, PCI root bridge
resources, DynamicTablesPkg CM objects, and Scorpi-specific ACPI content.

The hardware-info block is little-endian, 8-byte aligned, and versioned. The
VMM must zero-fill unused bytes. Firmware must reject a block with an invalid
magic, unsupported major version, invalid length, or invalid checksum.

```c
#define SCORPI_X64_HWINFO_FWCFG_FILE    "opt/scorpi/x64-hardware-info"
#define SCORPI_X64_HWINFO_MAGIC         "SXHI"
#define SCORPI_X64_HWINFO_MAJOR         1
#define SCORPI_X64_HWINFO_MINOR         0

enum scorpi_x64_profile {
	SCORPI_X64_PROFILE_LINUX_VIRTIO = 1,
	SCORPI_X64_PROFILE_WINDOWS_INBOX = 2,
};

enum scorpi_x64_hwinfo_entry_type {
	SCORPI_X64_ENTRY_RAM_RANGE = 1,
	SCORPI_X64_ENTRY_RESERVED_RANGE = 2,
	SCORPI_X64_ENTRY_CPU = 3,
	SCORPI_X64_ENTRY_APIC = 4,
	SCORPI_X64_ENTRY_PCIE_ECAM = 5,
	SCORPI_X64_ENTRY_PCI_WINDOW = 6,
	SCORPI_X64_ENTRY_DEVICE = 7,
	SCORPI_X64_ENTRY_FRAMEBUFFER = 8,
	SCORPI_X64_ENTRY_TPM = 9,
	SCORPI_X64_ENTRY_RESET = 10,
};

struct scorpi_x64_hwinfo_header {
	char     magic[4];
	uint16_t major;
	uint16_t minor;
	uint32_t header_size;
	uint32_t total_size;
	uint32_t checksum32;
	uint32_t flags;
	uint32_t profile;
	uint32_t entry_count;
	uint64_t entries_offset;
};

struct scorpi_x64_hwinfo_entry {
	uint16_t type;
	uint16_t flags;
	uint32_t size;
};
```

`checksum32` is the unsigned 32-bit sum of all bytes in `total_size` with the
checksum field treated as zero. Unknown entry types are skipped when `size` is
valid and 8-byte aligned.

Mandatory entries for v1:

- `RAM_RANGE`: one or more usable RAM ranges.
- `RESERVED_RANGE`: firmware, ECAM, LAPIC, IOAPIC, PCI windows, framebuffer,
  TPM, and reset/shutdown MMIO.
- `CPU`: one entry per vCPU with ACPI processor UID and APIC ID.
- `APIC`: LAPIC base `0xFEE00000`, IOAPIC base `0xFEC00000`, IOAPIC ID, first
  GSI, and GSI count.
- `PCIE_ECAM`: segment, start bus, end bus, ECAM base, and ECAM size.
- `PCI_WINDOW`: MMIO32 and MMIO64 apertures used by the PCI root bridge.
- `RESET`: MMIO reset/shutdown register block base, size, and command values.

Optional entries:

- `DEVICE`: boot-device hints and the selected default profile device list.
- `FRAMEBUFFER`: Scorpi GOP/RFB linear framebuffer details.
- `TPM`: TPM base, size, and interface type.

The VMM must not provide ACPI blobs for this platform.

## Timer And Clock Design

Scorpi X64 separates timekeeping into two platform responsibilities:

- Clocksource: a low-exit way for firmware and guests to compute elapsed time.
- Event timer: an interrupt source for one-shot and periodic timer events.

Modern profiles should prefer TSC and paravirtual clocksources over emulated
legacy hardware timers. The local APIC timer remains the baseline architectural
event timer and is owned by firmware/guest timer drivers, not delay-loop helper
libraries.

Baseline firmware and architectural timer policy:

- Expose a stable guest TSC frequency through CPUID leaf `0x15`.
- Use TSC-based firmware delay/stall code.
- Let `LocalApicTimerDxe` own the local APIC timer for UEFI timer events.
- Use KVM in-kernel LAPIC timer support for guest APIC timer delivery.
- Do not use HPET, PIT/i8254, RTC periodic interrupts, or ACPI PM timer by
  default.

Linux profile:

- Expose KVM pvclock/kvm-clock as the preferred Linux clocksource.
- Expose the stable kvmclock bit when KVM can provide a stable guest TSC.
- Add KVM steal-time accounting after the core boot path is stable.
- Keep LAPIC timer as the architectural event timer fallback.

Windows profile:

- Expose Hyper-V reference TSC page (`hv_time`) as the preferred Windows
  clocksource.
- Add Hyper-V synthetic timers for Windows event-timer optimization.
- Keep LAPIC timer as the architectural fallback.

Compatibility timers are not part of the default platform:

- HPET may be added later only as an explicit compatibility device if a real
  guest requirement appears.
- PIT/i8254, PIC, RTC periodic interrupts, and legacy ACPI PM timer are out of
  scope for the default Scorpi X64 machine.

## EDK2 Task List

- [ ] Add `OvmfPkg/Scorpi/ScorpiX64.dsc`.
- [ ] Add `OvmfPkg/Scorpi/ScorpiX64.fdf`.
- [ ] Build with `SMM_REQUIRE=FALSE` for v1.
- [ ] Include UEFI CPU, PCI bus, PCI host bridge, virtio, AHCI, xHCI, GOP,
  variable, secure boot, and boot manager support.
- [ ] Include Scorpi GOP/RFB support.
- [ ] Reuse or move Scorpi secure boot enrollment so it builds for X64 without
  ARM-only assumptions.
- [ ] Add `OvmfPkg/Include/IndustryStandard/ScorpiX64HwInfo.h` matching the
  v1 ABI in this document.
- [ ] Add Scorpi X64 hardware-info parser library using `QemuFwCfgLib` to read
  `opt/scorpi/x64-hardware-info`.
- [ ] Add Scorpi PlatformPei logic that builds memory/resource HOBs from
  hardware info, not CMOS/E820.
- [x] Use a TSC-based TimerLib for Scorpi X64 firmware delay/stall paths.
- [x] Keep APIC timer ownership in `LocalApicTimerDxe`, not Scorpi TimerLib.
- [ ] Add Scorpi PCI host bridge library exposing PCIe ECAM, one segment, bus 0
  root bridge, MMIO32/MMIO64 apertures, and no CF8/CFC dependency.
- [ ] Use DynamicTablesPkg for firmware-owned Scorpi X64 ACPI generation.
- [ ] Add a Scorpi X64 Configuration Manager that populates DynamicTablesPkg
  CM objects from Scorpi hardware info.
- [ ] Use X64/common DynamicTablesPkg generators where they fit: FADT, MADT,
  MCFG, SSDT PCIe, TPM2, SPCR, WSMT, and optional HPET.
- [ ] Provide Scorpi-specific DSDT/SSDT AML through a raw table object or a
  small custom generator for reset/shutdown and any Scorpi platform devices.
- [ ] Generate MADT, MCFG, FADT/FACS, DSDT, and optional TPM2/SPCR/BGRT.
- [ ] Describe PCIe root bridge resources in DSDT.
- [ ] Describe reset/shutdown through a modern Scorpi ACPI/MMIO device.
- [ ] Ensure FADT/DSDT do not require PIC/PIT/PM1/CMOS/LPC.
- [ ] Do not call OVMF `InstallQemuFwCfgTables()` on Scorpi X64.
- [ ] Validate generated ACPI against Linux and Windows.

## scorpi-hv Task List

- [ ] Enable `linux/x86_64/kvm` as a Meson-supported platform.
- [ ] Add `src/arch/x86/` platform entry code.
- [ ] Add `src/libvmm/kvm/x86/kvm_arch.c`.
- [ ] Create VMs with `KVM_CREATE_IRQCHIP`.
- [ ] Do not implement userspace PIC/PIT/ATPIC for this platform.
- [ ] Use LAPIC at `0xFEE00000` and IOAPIC at `0xFEC00000`.
- [ ] Add PCIe ECAM config-space emulation.
- [ ] Avoid CF8/CFC PCI config I/O.
- [ ] Implement x86 GSI routing with `KVM_SET_GSI_ROUTING`.
- [ ] Route INTx through `KVM_IRQ_LINE`.
- [ ] Route MSI/MSI-X through `KVM_SIGNAL_MSI`.
- [ ] Add `include/scorpi_x64_hwinfo.h` matching the v1 ABI in this document.
- [ ] Add Scorpi hardware-info blob production for firmware.
- [ ] Publish the hardware-info blob through fw_cfg as
  `opt/scorpi/x64-hardware-info`.
- [ ] Include RAM map, reserved MMIO ranges, vCPU/APIC IDs, IOAPIC info, PCIe
  ECAM, PCI windows, boot devices, framebuffer, TPM, and device profile.
- [x] Publish guest TSC frequency through CPUID leaf `0x15`.
- [ ] Verify KVM in-kernel LAPIC timer delivery with OVMF and Linux.
- [ ] Add KVM pvclock/kvm-clock exposure for the Linux profile.
- [ ] Expose kvmclock stable bit when guest TSC is stable.
- [ ] Add KVM steal-time accounting for the Linux profile.
- [ ] Add Hyper-V reference TSC page support for the Windows profile.
- [ ] Add Hyper-V synthetic timer support for the Windows profile.
- [ ] Keep HPET absent by default; add only as an explicit compatibility
  device if required.
- [ ] Do not publish VMM-built ACPI tables.
- [ ] Add Linux default profile using virtio devices only.
- [ ] Add Windows default profile using AHCI + xHCI + USB HID + USB network +
  Scorpi GOP.
- [ ] Keep device-profile selection explicit in config/API.

## Fast-Start Later

- [ ] Defer fast-start until normal UEFI Linux and Windows boot are stable.
- [ ] Define fast-start as a Linux-only profile.
- [ ] First optimize UEFI fast boot: skip UI delays, use deterministic
  BootOrder, reduce unnecessary drivers, and prefer virtio-only Linux devices.
- [ ] Later evaluate Firecracker-style direct kernel boot as a separate mode.
- [ ] If direct kernel boot is added, keep it separate from the firmware-ACPI
  UEFI path because bypassing firmware conflicts with "ACPI built entirely in
  firmware."
- [ ] Reuse the same Scorpi hardware-info schema for direct boot where possible,
  but do not let fast-start requirements change the v1 firmware contract.

## Compatibility Requirements

- [ ] Linux boots through UEFI using the `linux-virtio` profile by default.
- [ ] Linux discovers CPUs through MADT, PCI through MCFG, and devices through
  PCIe virtio.
- [ ] Windows boots through UEFI without virtio driver installation.
- [ ] Windows storage works through AHCI inbox driver.
- [ ] Windows keyboard/mouse works through USB HID inbox drivers.
- [ ] Windows networking works through an inbox-supported USB network device.
- [ ] KVM in-kernel LAPIC/IOAPIC is the only interrupt-controller
  implementation.
- [ ] Guest must not depend on i440fx, Q35, PIC, PIT, ISA, LPC, CMOS, CF8/CFC,
  or VMM-provided ACPI.

## Test Plan

- [ ] Build Scorpi X64 EDK2 firmware in `DEBUG` and `RELEASE`.
- [ ] Boot UEFI shell with 1, 2, and 4 vCPUs.
- [ ] Verify UEFI shell serial input after idle to catch broken firmware timer
  events.
- [ ] Boot Linux installer with default virtio-only profile.
- [ ] Verify Linux selects kvm-clock after KVM pvclock is implemented.
- [ ] Verify Linux LAPIC timer fallback works when pvclock is disabled.
- [ ] Dump Linux ACPI with `acpidump` and validate with `iasl`.
- [ ] Verify Linux sees LAPIC, IOAPIC, MCFG, virtio devices, and expected memory
  map.
- [ ] Boot Windows installer without virtio drivers.
- [ ] Verify Windows uses Hyper-V reference TSC page after Hyper-V clock
  support is implemented.
- [ ] Verify Windows synthetic timer delivery after Hyper-V timer support is
  implemented.
- [ ] Install Windows to AHCI disk.
- [ ] Verify Windows keyboard, mouse, display, storage, and network.
- [ ] Verify MSI/MSI-X and INTx delivery.
- [ ] Verify ACPI reset/shutdown path.
- [ ] Measure UEFI Linux boot time as the baseline for later fast-start work.

## Assumptions

- Target Windows is modern 64-bit UEFI Windows.
- Linux should default to virtio-only devices.
- Windows should not require installing virtio drivers.
- Compatibility devices are allowed when they do not imply legacy chipset or
  interrupt-controller emulation.
- Fast-start is v2+ and Linux-only unless a later requirement says otherwise.
