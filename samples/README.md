# Samples

These examples use the FreeBSD ARM64 boot-only ISO referenced by [run.sh](/Users/alexf/work/scorpi/run.sh).

Expected files in the repo root:

- `./firmware/u-boot.bin`
- `./FreeBSD-14.2-RELEASE-arm64-aarch64-bootonly.iso`

You can fetch the ISO with:

```sh
./run.sh
```

Then launch the YAML sample:

```sh
./builddir/scorpi -f ./samples/freebsd_vm.yaml
```

Or build and run the one-file API sample:

```sh
meson compile -C builddir freebsd_api_sample
./builddir/samples/freebsd_api_sample
```

## Scorpi x86 OVMF

The x86 sample expects the Scorpi OVMF build output from the sibling edk2
checkout:

```sh
../edk2/Build/ScorpiX64/DEBUG_GCC5/FV/SCORPI_EFI.fd
```

Launch it with YAML only:

```sh
./builddir/scorpi -f ./samples/ovmf_x86_vm.yaml
```
