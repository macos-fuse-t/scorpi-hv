project('scorpi', 'c', default_options: ['c_std=c11'])

add_project_arguments('-Wno-pointer-sign', language: 'c')

zlib_dep = dependency('zlib', required: true)
slirp_dep = dependency('slirp', required: true, static: false)
libwebsockets_dep = dependency('libwebsockets', required: true)
threads_dep = dependency('threads')

codesign_identity = run_command('sh', '-c', 'echo $CODESIGN_IDENTITY').stdout().strip()

if host_machine.system() == 'darwin'
    hv_framework = [
        '-framework', 'Hypervisor',
        '-framework', 'VMNet' ]
    add_project_link_arguments(hv_framework, language: 'c')
    add_project_link_arguments(['-lresolv'], language: 'c' )
    #add_project_link_arguments(['-ld64'], language: 'c' )
endif

arch = host_machine.cpu_family()

inc = include_directories(
    'include',
    'libyaml/include',
    'libnv',
    'libfdt',
    'libutil',
    'mjson',
    'src/libscorpi',
)

subdir('libyaml')
subdir('libnv')
subdir('libfdt')
subdir('libutil')
subdir('mjson')
subdir('src/libscorpi')
subdir('tests')
subdir('util/console')

sources = files(
    'src/compat.c',
    'src/bootrom.c',
    'src/config.c',
    'src/bhyverun.c',
    'src/mem.c',
    'src/pci_emul.c',
    'src/acpi.c',
    'src/acpi_device.c',
    'src/basl.c',
    'src/tpm_device.c',
    'src/tpm_emul_swtpm.c',
    'src/tpm_intf_crb.c',
    'src/tpm_intf_tis.c',
    'src/tpm_ppi_qemu.c',
    'src/mevent.c',
    'src/qemu_fwcfg.c',
    'src/qemu_loader.c',
    'src/scorpi_cli.c',
    'src/scorpi_runtime_bridge.c',
    'src/rtc_pl031.c',
    'src/uart_pl011.c',
    'src/uart_backend.c',
    'src/vmgenc.c',
    'src/pci_hostbridge.c',
    'src/pci_ahci.c',
    'src/block_if.c',
    'src/virtio.c',
    'src/pci_virtio_block.c',
    'src/pci_virtio_net.c',
    'src/pci_virtio_rnd.c',
    'src/pci_82545.c',
    'src/net_backends.c',
    'src/rfb.c',
    'src/pci_fbuf.c',
    'src/bhyvegc.c',
    'src/console.c',
    'src/sockstream.c',
    'src/iov.c',
    'src/net_utils.c',
    'src/cnc.c',
    'src/pci_xhci.c',
    'src/usb_emul.c',
    'src/usb_mouse.c',
    'src/usb_kbd.c',
    'src/smbiostbl.c',
    'src/slirp_net_backend.c',
    'src/sock_net_backend.c',
    'src/usb_net.c',
    'src/pci_virtio_gpu.c',
    'src/pci_virtio_console.c',
)

# Set architecture-specific source
if arch == 'x86_64'
    arch_src = files(
        'src/arch/x86/bhyverun.c'
    )
elif arch == 'aarch64'
    arch_src = files(
        'src/arch/arm64/bhyverun.c',
        'src/arch/arm64/fdt.c',
        'src/arch/arm64/pci_irq.c',
        'src/arch/arm64/vmexit.c',
    )
else
    error('Unsupported architecture: ' + arch)
endif

if host_machine.system() == 'darwin'
    arch_src += files(
        'src/libvmm/hvf/vmmapi.c',
        'src/libvmm/hvf/vmm.c',
        'src/libvmm/hvf/vgic.c',
        'src/libvmm/hvf/vmm_instruction_emul.c',
        #'src/vmnet_net_backend.c',
    )
endif

sources += arch_src

scorpi_executable=executable(
  'scorpi', 
  sources,
  dependencies: [scorpi_kit_dep, libnv_dep, libfdt_dep, libutil_dep, libmjson_dep, zlib_dep, slirp_dep, libwebsockets_dep, threads_dep],
  include_directories: inc,
)

# Add codesigning step for macOS
if host_machine.system() == 'darwin'
    codesign = custom_target(
        'codesign-scorpi',
        input: scorpi_executable,
        output: 'scorpi-signed',
        command: [
            'codesign',
            '--entitlements', meson.project_source_root() + '/scorpi.entitlements',
            '--sign', codesign_identity,
            '--force',
            '@INPUT@'
        ],
        build_by_default: true
    )
endif
