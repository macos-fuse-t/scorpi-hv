libyaml_inc = include_directories('include')

libyaml_src = files(
  'src/api.c',
  'src/dumper.c',
  'src/emitter.c',
  'src/loader.c',
  'src/parser.c',
  'src/reader.c',
  'src/scanner.c',
  'src/writer.c',
)

libyaml = static_library(
  'yaml',
  libyaml_src,
  include_directories: [libyaml_inc],
  c_args: [
    '-DYAML_DECLARE_STATIC',
    '-DYAML_VERSION_MAJOR=0',
    '-DYAML_VERSION_MINOR=2',
    '-DYAML_VERSION_PATCH=5',
    '-DYAML_VERSION_STRING="0.2.5"',
  ],
)

libyaml_dep = declare_dependency(
  include_directories: libyaml_inc,
  link_with: libyaml,
)
