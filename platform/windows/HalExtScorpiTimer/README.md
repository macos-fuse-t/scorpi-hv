# Windows ARM64 profiling timer

This HAL extension provides periodic CPU sampling through Scorpi's synthetic
timer support. Enable timers in the hypervisor YAML:

```yaml
cpu:
  cores: 4
  nested-virt: false
  hyperv:
    crash: true
    stimers: true
```

Synthetic timers default to disabled. Windows sampling requires matching Scorpi
EDK2 firmware and this extension installed in the guest. See the
[Hyper-V feature settings](../../../docs/arm64-hyperv-features.md).

## Build

Use LLVM's Windows ARM64 COFF target and the Microsoft WDK ARM64 libraries:

```sh
CLANG=/path/to/clang LLD_LINK=/path/to/lld-link ./build.sh \
  /path/to/wdk-headers /path/to/wdk/km/ARM64 /path/to/output
```

The headers directory must contain `km`, `shared` and `km/crt`. The output
contains `HalExtScorpiTimer.dll` and its installer.

## Development installation

In a guest with test signing already enabled, run as administrator:

```powershell
.\install-test.ps1 -CertificateDirectory 'C:\Program Files (x86)\Windows Kits\10\Tools\certificates'
```

Reboot the guest to load the extension. Installation uses the WDK OEM HAL
Extension certificate chain and requires currently valid certificates. The
installer does not change test-signing or Secure Boot settings. The WDK
certificate is for development use only.
