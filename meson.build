project('scorpi', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11'])

cc = meson.get_compiler('c')
host_system = host_machine.system()
arch = host_machine.cpu_family()

add_project_arguments('-Wno-pointer-sign', language: 'c')
if cc.has_argument('-Wno-format')
  add_project_arguments('-Wno-format', language: 'c')
endif
add_project_arguments('-D_GNU_SOURCE', language: 'c')

macos_min_version = '15.0'
if host_system == 'darwin'
  macos_min_flag = '-mmacosx-version-min=' + macos_min_version
  if cc.has_argument(macos_min_flag)
    add_global_arguments(macos_min_flag, language: 'c')
    add_global_link_arguments(macos_min_flag, language: 'c')
  endif
endif

zlib_dep = dependency('zlib', required: true)
uuid_dep = dependency('uuid', required: true)
threads_dep = dependency('threads')
platform_deps = []
if host_system == 'linux'
    platform_deps += cc.find_library('rt', required: false)
endif

libslirp_proj = subproject(
    'libslirp',
    default_options: ['default_library=static'])
slirp_dep = libslirp_proj.get_variable('libslirp_libdep')

libwebsockets_build_dir = meson.current_build_dir() / 'libwebsockets-build'
libwebsockets_static = custom_target(
    'libwebsockets-static',
    output: ['libwebsockets.a', 'lws_config.h'],
    command: [
        files('build-aux/build-libwebsockets.sh'),
        meson.project_source_root() / 'subprojects/libwebsockets',
        libwebsockets_build_dir,
        '@OUTPUT0@',
        '@OUTPUT1@',
        host_system,
        macos_min_version,
    ],
)
libwebsockets_dep = declare_dependency(
    sources: libwebsockets_static,
    compile_args: [
        '-I' + meson.project_source_root() / 'subprojects/libwebsockets/include',
        '-I' + meson.current_build_dir(),
    ],
)

codesign_identity = run_command('sh', '-c', 'echo $CODESIGN_IDENTITY').stdout().strip()

hypervisor_backend = get_option('hypervisor')

if hypervisor_backend == 'auto'
    if host_system == 'darwin' and arch == 'aarch64'
        hypervisor_backend = 'hvf'
    elif host_system == 'linux' and (arch == 'aarch64' or arch == 'x86_64')
        hypervisor_backend = 'kvm'
    else
        error('No default hypervisor backend for ' + host_system + '/' + arch)
    endif
endif

if hypervisor_backend == 'hvf'
    if host_system != 'darwin' or arch != 'aarch64'
        error('HVF backend requires darwin/aarch64')
    endif
elif hypervisor_backend == 'kvm'
    if host_system != 'linux' or (arch != 'aarch64' and arch != 'x86_64')
        error('KVM backend requires linux/aarch64 or linux/x86_64')
    endif
else
    error('Unsupported hypervisor backend: ' + hypervisor_backend)
endif

message('Hypervisor backend: ' + hypervisor_backend)

if host_system == 'darwin'
    hv_framework = [
        '-framework', 'Hypervisor',
        '-framework', 'VMNet' ]
    add_project_arguments('-fblocks', language: 'c')
    add_project_link_arguments(hv_framework, language: 'c')
    #add_project_link_arguments(['-ld64'], language: 'c' )
endif

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
    'src/bhyvegc.c',
    'src/console.c',
    'src/sockstream.c',
    'src/iov.c',
    'src/net_utils.c',
    'src/pci_xhci.c',
    'src/usb_emul.c',
    'src/usb_mouse.c',
    'src/usb_kbd.c',
    'src/usb_serial.c',
    'src/smbiostbl.c',
    'src/slirp_net_backend.c',
    'src/sock_net_backend.c',
    'src/usb_net.c',
    'src/pci_virtio_gpu.c',
    'src/pci_virtio_console.c',
    'src/cnc.c',
)

if get_option('fbuf')
    runtime_core_sources += files('src/pci_fbuf.c')
endif

host_src = files()
if host_system == 'darwin'
    host_src += files(
        'src/mevent.c',
    )
elif host_system == 'linux'
    host_src += files(
        'src/mevent_linux.c',
    )
else
    error('Unsupported host system: ' + host_system)
endif

# Set architecture-specific source
if arch == 'x86_64'
    arch_src = files(
        'src/arch/x86/bootrom_arch.c',
        'src/arch/x86/bhyverun.c',
        'src/arch/x86/debugcon.c',
        'src/arch/x86/inout.c',
        'src/arch/x86/pci_uart.c',
        'src/arch/x86/pci_irq.c',
        'src/arch/x86/qemu_fwcfg_arch.c',
        'src/arch/x86/rtc.c',
        'src/arch/x86/scorpi_hwinfo.c',
        'src/arch/x86/vmexit.c',
        'src/uart_emul.c',
    )
elif arch == 'aarch64'
    arch_src = files(
        'src/arch/arm64/bootrom_arch.c',
        'src/arch/arm64/bhyverun.c',
        'src/arch/arm64/fdt.c',
        'src/arch/arm64/pci_irq.c',
        'src/arch/arm64/qemu_fwcfg_arch.c',
        'src/arch/arm64/vmexit.c',
    )
else
    error('Unsupported architecture: ' + arch)
endif

backend_src = files()
if hypervisor_backend == 'hvf'
    backend_src += files(
        'src/libvmm/hvf/vmmapi.c',
        'src/libvmm/hvf/vmm.c',
        'src/libvmm/hvf/vgic.c',
        'src/vmnet_net_backend.c',
    )
elif hypervisor_backend == 'kvm'
    if arch == 'aarch64'
        backend_src += files(
            'src/libvmm/common/vmm_instruction_emul.c',
            'src/libvmm/kvm/vmmapi.c',
            'src/libvmm/kvm/arm64/kvm_arch.c',
        )
    elif arch == 'x86_64'
        backend_src += files(
            'src/libvmm/kvm/x86/vmmapi.c',
            'src/libvmm/kvm/x86/vmm_instruction_emul.c',
        )
    endif
endif

runtime_core_sources += host_src + arch_src + backend_src

scorpi_image_executable = executable(
  'scorpi-image',
  files(
    'src/compat.c',
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
