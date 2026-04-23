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
