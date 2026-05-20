# Scorpi - A Modern Lightweight General-Purpose Hypervisor

## Overview

Scorpi is a modern, lightweight, general-purpose hypervisor designed to be an alternative to QEMU.

### Key Features

- **Modern**: Implements only modern devices, primarily VirtIO-based, avoiding legacy emulations.
- **Lightweight**: Built on FreeBSD Bhyve and written in C, with minimal code base for emulating devices.
- **General-Purpose**: Supports headless and graphical VMs, EFI boot loader, and ACPI. Can run  Linux and Windows VMs.
- **Modular**: Designed to be used as an API in other applications and services. Graphics, UI and user input are separate modules, and networking can be modularized as well.

## Platform Support

Currently, Scorpi runs on Linux/KVM X86/ARM64 and on Mac ARM64 using Apple's Hypervisor Framework.

## Available Bootloaders

1. **U-Boot** (ARM only) - Fast and compact but lacks some advanced features such as ACPI and graphics. Best used for headless VMs that require fast start.\
   [Source Code](https://github.com/macos-fuse-t/u-boot)

2. **EDK2 UEFI** - Full-featured bootloader that provides ACPI support, frame buffer, and a variety of boot device drivers.\
   [Source Code](https://github.com/macos-fuse-t/edk2)

## Build and run

Build and run scorpi-hv and samples with

```sh
./build.sh
```

Run a FreeBSD VM:

```sh
./run.sh
```

Or launch it with the YAML config:

```sh
./builddir/scorpi-hv -f ./samples/freebsd_vm.yaml
```

## Running Linux VMs

1. Download an ISO that supports machine's architecture.
2. Create an empty disk with:
   ```sh
   mkfile -n [size] [img_file]
   ```
3. Example YAML configuration:
   ```sh
   cat > linux-vm.yaml <<'EOF'
   name: vm1
   cpu: 2
   memory: 2G
   console: stdio
   bootrom: ./firmware/SCORPI_EFI.fd

   devices:
     pci:
       - device: hostbridge
         slot: 0

       - device: xhci
         slot: 1
         id: xhci0

       - device: virtio-blk
         slot: 2
         path: [img_file]

       - device: virtio-blk
         slot: 3
         path: [iso_file]
         ro: true

       - device: virtio-net
         slot: 4
         backend: slirp

       - device: virtio-gpu
         slot: 5
         hdpi: on

     usb:
       - device: kbd
       - device: tablet

     lpc:
       - device: vm-control
         path: /tmp/vm_sock
   EOF
   ./builddir/scorpi-hv -f ./linux-vm.yaml
   ```
   To use a graphical viewer (macOS), refer to the following reference project: [ScorpiViewer](https://github.com/macos-fuse-t/ScorpiViewer)

## Running a Windows VM on macOS

1. Download a Windows ARM64 ISO
2. Create an empty disk with:
   ```sh
   mkfile -n [size] [img_file]
   ```
3. Launch the TPM emulator `swtpm` and expose a Unix socket for Scorpi:
   ```sh
   mkdir -p /tmp/scorpi-tpm/state
   swtpm socket --tpm2 --flags not-need-init,startup-clear --tpmstate dir=/tmp/scorpi-tpm/state --server type=unixio,path=/tmp/scorpi-tpm/swtpm.sock
   ```
4. Example YAML configuration and launch:
   ```sh
   cat > windows-vm.yaml <<'EOF'
   name: vm1
   cpu: 2
   memory: 4G
   bootrom: ./firmware/SCORPI_EFI_ARM.fd
   bootvars: ./firmware/SCORPI_VARS_ARM.fd

   devices:
     pci:
       - device: hostbridge
         slot: 0

       - device: xhci
         slot: 1
         id: xhci0

       - device: ahci
         slot: 2
         port.0.type: hd
         port.0.path: [img_file]
         port.1.type: cd
         port.1.path: [iso]
         port.1.ro: true

       - device: virtio-gpu
         slot: 5
         fb: on

     usb:
       - device: kbd
       - device: tablet
       - device: net
         backend: slirp

     lpc:
       - device: vm-control
         path: /tmp/vm_sock

       - device: tpm
         type: swtpm
         path: /tmp/scorpi-tpm/swtpm.sock
         version: 2.0
         intf: tis
   EOF
   ./builddir/scorpi-hv -f ./windows-vm.yaml
   ```
5. Run ScorpiViewer.

## Future Roadmap

- Implement and add missing features (file sharing, copy/paste support)
- Add Windows DirectX 12 display driver.
- Extend support to RISC-V and other platforms.

## Releated Projects
[U-Boot bootloader](https://github.com/macos-fuse-t/u-boot)  
[EDK2 bootloader](https://github.com/macos-fuse-t/edk2)  
[Scorpi Viewer](https://github.com/macos-fuse-t/ScorpiViewer)  

## Licensing

Scorpi is released under a **permissive license**, providing flexibility for various use cases.

## Get Involved

Contributions and feedback are welcome! Stay tuned for updates as Scorpi evolves into a powerful and versatile hypervisor.

For inquiries, contact **Alex Fishman** at [alex@fuse-t.org](mailto\:alex@fuse-t.org).
