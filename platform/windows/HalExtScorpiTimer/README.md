# Windows ARM64 profiling timer

The `cpu.profile-timer` HV option (default off, non-nested guests only) enables
the host synthetic timers and adds a firmware description for STIMER1/PPI22.
With the option omitted or false, no timer worker starts, no synthetic timer
capabilities are advertised, and no profiling-timer device is described.
The option also requires `cpu.hyperv=true` (the existing default).
Scorpi EDK2 installs its CSRT/SSDT. This
HAL extension registers a periodic, per-processor hypervisor timer through the
WDK HAL-extension API. Each VP programs its own STIMER1. STIMER0 remains owned
by the built-in Windows HAL plugin. No kernel patches are used.

Opt in explicitly under `cpu` in the VM YAML:

```yaml
cpu:
  cores: 4
  hyperv: true          # default: true
  profile-timer: true   # default: false
  nested-virt: false
```

The CLI equivalent is `-o cpu.profile-timer=true`. Both options use the `cpu`
namespace; the earlier `arm64.*` option names are no longer read.
Matching Scorpi EDK2 firmware
with `ScorpiProfileTimerDxe` and the installed HAL extension are required for
Windows sampling. Building this repository does not install the extension,
replace VM firmware, or enable the option in any VM configuration.

Build with LLVM's Windows ARM64 COFF target and the Microsoft WDK ARM64 libraries:

```sh
CLANG=/path/to/clang LLD_LINK=/path/to/lld-link ./build.sh \
  /path/to/wdk-headers /path/to/wdk/km/ARM64 /path/to/output
```

The headers directory must contain `km`, `shared`, and `km/crt`. The output
contains the DLL and installer; WDK certificates are not copied or committed.

In a disposable test guest with test signing already enabled, run as administrator:

```powershell
.\install-test.ps1 -CertificateDirectory 'C:\Program Files (x86)\Windows Kits\10\Tools\certificates'
```

Then reboot. HAL extensions need the WDK OEM **HAL Extension** test certificate
and its chain. A generic self-signed code-signing certificate can pass
Authenticode verification while winload rejects it with `0xC0000428`. The
installer checks certificate dates and does not change test-signing or Secure
Boot settings. Do not use the public WDK test certificate for production.

