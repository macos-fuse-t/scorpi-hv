pkg = import('pkgconfig')

libscorpi_src = files(
  'scorpi.c',
)

scorpi_kit_sources = libscorpi_src + runtime_core_sources
scorpi_kit_private_deps = [
  libnv_dep,
  libfdt_dep,
  libutil_dep,
  libmjson_dep,
  libyaml_dep,
  zlib_dep,
  slirp_dep,
  libwebsockets_dep,
  threads_dep,
]

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

pkg.generate(
  scorpi_kit,
  name: 'scorpi-kit',
  filebase: 'scorpi-kit',
  description: 'Scorpi VM construction and launch library',
  subdirs: '',
)
