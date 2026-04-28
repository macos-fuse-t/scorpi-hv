project('scorpi', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11'])

add_project_arguments('-Wno-pointer-sign', language: 'c')

cmake = import('cmake')

zlib_dep = dependency('zlib', required: true)
threads_dep = dependency('threads')

libslirp_proj = subproject(
    'libslirp',
    default_options: ['default_library=static'])
slirp_dep = libslirp_proj.get_variable('libslirp_libdep')

libwebsockets_opts = cmake.subproject_options()
libwebsockets_opts.add_cmake_defines({
    'LWS_WITH_SHARED': false,
    'LWS_WITH_STATIC': true,
    'LWS_WITH_SSL': false,
    'LWS_WITHOUT_TESTAPPS': true,
})
libwebsockets_opts.set_install(false)
libwebsockets_proj = cmake.subproject(
    'libwebsockets',
    options: libwebsockets_opts)
libwebsockets_dep = libwebsockets_proj.dependency('websockets')

codesign_identity = run_command('sh', '-c', 'echo $CODESIGN_IDENTITY').stdout().strip()

if host_machine.system() == 'darwin'
    hv_framework = [
        '-framework', 'Hypervisor',
        '-framework', 'VMNet' ]
    add_project_arguments('-fblocks', language: 'c')
    add_project_link_arguments(hv_framework, language: 'c')
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

runtime_core_sources = files(
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
    'src/scorpi_runtime_bridge.c',
    'src/scorpi_image.c',
    'src/scorpi_image_chain.c',
    'src/scorpi_image_qcow2.c',
    'src/scorpi_image_raw.c',
    'src/scorpi_image_sco.c',
    'src/scorpi_image_uri.c',
    'src/scorpi_crc32c.c',
    'src/scorpi_host_sparse.c',
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
        'src/vmnet_net_backend.c',
    )
endif

runtime_core_sources += arch_src

scorpi_image_executable = executable(
  'scorpi-image',
  files(
    'src/scorpi_image_tool.c',
    'src/scorpi_image.c',
    'src/scorpi_image_chain.c',
    'src/scorpi_image_qcow2.c',
    'src/scorpi_image_raw.c',
    'src/scorpi_image_sco.c',
    'src/scorpi_image_uri.c',
    'src/scorpi_crc32c.c',
    'src/scorpi_host_sparse.c',
  ),
  dependencies: [libutil_dep, threads_dep, zlib_dep],
  include_directories: [inc, include_directories('src')],
)

subdir('src/libscorpi')
subdir('tests')
subdir('util/console')
subdir('samples')

scorpi_executable=executable(
  'scorpi-hv',
  files(
    'src/scorpi_main.c',
    'src/scorpi_cli.c',
  ),
  dependencies: [scorpi_kit_dep],
  include_directories: inc,
)

# Add codesigning step for macOS
if host_machine.system() == 'darwin'
    codesign = custom_target(
        'codesign-scorpi-hv',
        input: scorpi_executable,
        output: 'scorpi-hv-signed',
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
