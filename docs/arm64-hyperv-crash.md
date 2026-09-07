# ARM64 Windows crash notifications

The HVF backend implements the Hyper-V ARM64 discovery and crash-register
subset. It is enabled by default (`arm64.hyperv=true`). It does not advertise
synthetic interrupts, timers, VMBus, or enlightened TLB/IPI operations.

Windows ARM64 build 26100 also performs these mandatory boot registrations,
independently of the optional feature bits:

- `0x90015`: partition information page (guest GPA plus enable bit). The page
  is validated and initialized with no pending hypervisor actions.
- `0x90016`: per-VP enlightenment opt-in.
- `0x40400`: synthetic exception vector base, separate from `VBAR_EL1`.

These boot interfaces are implemented and reset with the VM. Optional discovery
registers `0x204`–`0x209` return zero capabilities. Unsupported register errors
are logged once per register, up to 16 distinct registers per boot.

The initial crash-only implementation rejected `0x90015` in
`HvlpInitializePartitionInfoPage`. The early exception recursively entered
`RtlRaiseStatus` until Windows bugchecked with `0x2B`. The subsequent boot
registrations above were identified from the captured kernel and live retries.
With them implemented, Windows 11 build 26100 boots and remote PowerShell
responds. Captures are retained in `.cache/hyperv-boot-failure/`.

Both the SMCCC interface and Windows' legacy HVC #1 register ABI are supported.
Windows also needs the matching EDK2 change in
`ArmVirtPkg/KvmtoolCfgMgrDxe/ConfigurationManager.c`: it queries the running
hypervisor's UID and supplies the FADT `MsHyperV` identity only on a match.
Use the rebuilt hypervisor **and** rebuilt Scorpi ARM firmware on the next VM
launch. Rebooting Windows inside an already running old hypervisor does not
load the new host code.

On a crash notification, `PRINTLN` writes one line to the normal VM log:

```
Hyper-V guest crash: vcpu=2 guestOsId=0x... control=0x8000000000000000 P0=0x... P1=0x... P2=0x... P3=0x... P4=0x...
```

For Windows, P0 is the bugcheck code and P1–P4 are its parameters. For `win`
in this setup the destination is
`~/Library/Logs/Scorpi/vms/vm_snBQdVAeondt.log`. Discovery and guest OS ID writes
also produce short log lines. Discovery alone can come from firmware; a
Windows guest OS ID write provides evidence that Windows reached the interface.

Notification handling returns success and lets Windows continue its dump
procedure. It does not create a Windows memory dump, force a bugcheck, stop the
VM, or change Windows dump/reboot settings. A hang that never reaches Windows'
crash notification path will not produce a crash line.

Validation:

```
ninja -C build-macos-arm64
meson test -C build-macos-arm64 --no-rebuild --print-errorlogs arm64_hyperv_test
```

Build the firmware from the EDK2 repository with LLVM 18:

```
export PATH=/opt/homebrew/opt/llvm@18/bin/:$PATH
export CLANGDWARF_BIN=/opt/homebrew/opt/llvm@18/bin/
. ./edksetup.sh
build -p ArmVirtPkg/ArmVirtScorpi.dsc -a AARCH64 -b DEBUG -t CLANGDWARF
```

The code image is `Build/ArmVirtScorpi-AARCH64/DEBUG_CLANGDWARF/FV/SCORPI_EFI.fd`.
Use it as the VM boot ROM; preserve the existing variable store and guest disk.

The protocol test covers both calling conventions, discovery, capability bits,
register access, repetition progress, malformed input, state reset/isolation,
and the emitted crash log. The failed Windows boots also exercised the live
crash notification path and recorded bugcheck `0x2B` with its trap-frame address.
Successful notification does not establish that Windows completed a memory dump.
