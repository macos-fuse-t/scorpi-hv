console_src = files('tty.c', 'websock_client.c')

console_executable = executable(
  'scorpi_console',
  console_src,
  dependencies: [libwebsockets_dep]
)

if host_machine.system() == 'darwin'
    codesign = custom_target(
        'codesign-scorpi',
        input: console_executable,
        output: 'scorpi-console',
        command: [
            'codesign',
            '--sign', codesign_identity,
            '--force',
            '@INPUT@'
        ],
        build_by_default: true
    )
endif
