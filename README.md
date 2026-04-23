# Scorpi - A Modern Lightweight General-Purpose Hypervisor

## Overview

Scorpi is a modern, lightweight, general-purpose hypervisor designed to be an alternative to QEMU.

### Key Features

- **Modern**: Implements only modern devices, primarily VirtIO-based, avoiding legacy emulations.
- **Lightweight**: Built on FreeBSD Bhyve and written in C, with minimal code base for emulating devices.
- **General-Purpose**: Supports headless and graphical VMs, EFI boot loader, and ACPI. Can run  Linux and Windows VMs.
- **Modular**: Designed to be used as an API in other applications and services. Graphics, UI and user input are separate modules, and networking can be modularized as well.

## Platform Support

Currently, Scorpi runs on Mac ARM64 using Apple's Hypervisor Framework. The plan is to expand support to:

- **Linux x86 and ARM** using **KVM**
- **Additional architectures**, including **RISC-V**

## Available Bootloaders

1. **U-Boot** - Fast and compact but lacks some advanced features such as ACPI and graphics. Best used for headless VMs that require fast start.\
   [Source Code](https://github.com/macos-fuse-t/u-boot)

2. **EDK2 UEFI** - Full-featured bootloader that provides ACPI support, frame buffer, and a variety of boot device drivers.\
   [Source Code](https://github.com/macos-fuse-t/edk2)

## Build and run

Build and run the scorpi kit and samples with

```sh
meson compile -C builddir
```

Run FreeBSD VM:

```sh
./run.sh
```

Or launch it with the YAML config:

```sh
./builddir/scorpi -f ./samples/freebsd_vm.yaml
```

## Running Linux VMs

1. Download an ISO that supports ARM<sub>64</sub> architecture.
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
   ./builddir/scorpi -f ./linux-vm.yaml
   ```
   To use a graphical viewer, refer to the following reference project: [ScorpiViewer](https://github.com/macos-fuse-t/ScorpiViewer)

## Running a Windows VM

1. Download a Windows ARM<sub>64</sub> ISO
2. Create an empty disk with:
   ```sh
   mkfile -n [size] [img_file]
   ```
3. Launch the TPM emulator `swtpm` and expose a Unix socket for Scorpi:
   ```sh
   mkdir -p /tmp/scorpi-tpm
   swtpm socket --tpm2 --flags startup-clear --tpmstate dir=/tmp/scorpi-tpm/state --server type=unixio,path=/tmp/scorpi-tpm/swtpm.sock
   ```
4. Example YAML configuration and launch:
   ```sh
   cat > windows-vm.yaml <<'EOF'
   name: vm1
   cpu: 2
   memory: 4G
   bootrom: ./firmware/SCORPI_EFI.fd
   bootvars: ./firmware/SCORPI_VARS.fd

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
   ./builddir/scorpi -f ./windows-vm.yaml
   ```
5. Run ScorpiViewer.

## Future Roadmap

- Implement and add missing features (file sharing, copy/paste support)
- Implement Linux support on top of KVM.
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
