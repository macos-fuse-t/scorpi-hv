sample_sources = files('freebsd_api.c')

freebsd_api_sample = executable(
  'freebsd_api_sample',
  sample_sources,
  dependencies: [scorpi_kit_dep],
  include_directories: inc,
)

if host_machine.system() == 'darwin'
    custom_target(
        'codesign-freebsd_api_sample',
        input: freebsd_api_sample,
        output: 'freebsd_api_sample-signed',
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
