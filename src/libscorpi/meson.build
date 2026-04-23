libscorpi_src = files(
  'scorpi.c',
)

scorpi_kit = static_library(
  'scorpi_kit',
  libscorpi_src,
  include_directories: inc,
  dependencies: [libnv_dep, libyaml_dep, libutil_dep],
)

scorpi_kit_dep = declare_dependency(
  include_directories: inc,
  link_whole: scorpi_kit,
  dependencies: [scorpi_runtime_core_dep],
)
