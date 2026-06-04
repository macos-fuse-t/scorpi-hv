pkg = import('pkgconfig')

libscorpi_src = files(
  'scorpi.c',
)

scorpi_kit_sources = libscorpi_src + runtime_core_sources + [libwebsockets_static]
scorpi_kit_private_deps = [
  libnv_dep,
  libfdt_dep,
  libutil_dep,
  libmjson_dep,
  libyaml_dep,
  zlib_dep,
  slirp_dep,
  threads_dep,
  uuid_dep,
] + platform_deps + [libwebsockets_dep]

scorpi_kit = shared_library(
  'scorpi_kit',
  scorpi_kit_sources,
  include_directories: inc,
  dependencies: scorpi_kit_private_deps,
  install: true,
)

scorpi_kit_dep = declare_dependency(
  include_directories: include_directories('../../include'),
  link_with: scorpi_kit,
)

install_headers('../../include/scorpi.h')
install_headers(
  '../../include/support/endian.h',
  subdir: 'support',
)
install_headers(
  '../../include/scorpi/protocol/virtio_gpu.h',
  '../../include/scorpi/protocol/virtio_ring.h',
  '../../include/scorpi/protocol/virtio_vhost.h',
  subdir: 'scorpi/protocol',
)

pkg.generate(
  scorpi_kit,
  name: 'scorpi-kit',
  filebase: 'scorpi-kit',
  description: 'Scorpi VM construction and launch library',
  subdirs: '',
)

pkg.generate(
  name: 'scorpi-protocol',
  filebase: 'scorpi-protocol',
  description: 'Scorpi public protocol headers',
  subdirs: '',
)
