test_src = files('nvlist_test.c')

nv_test_executable = executable(
  'nvlist_test',  # Name of the executable
  test_src,
  dependencies: libnv_dep,
  include_directories: inc
)

test('nvlist_test', nv_test_executable)

dtb = custom_target('test_dtb',
    input: 'test.dts',
    output: 'test.dtb',
    command: ['dtc', '-I', 'dts', '-O', 'dtb', '-o', '@OUTPUT@', '@INPUT@']
)

fdt_test_src = files('fdt_test.c')

fdt_test_executable = executable(
  'fdt_test', 
  fdt_test_src,
  dependencies: libfdt_dep,
  include_directories: inc,
)

test_env = environment()
test_env.set('TEST_DTB_FILE', meson.current_build_dir() / 'test.dtb')
test('fdt_test', fdt_test_executable, env: test_env, depends: dtb)

scorpi_header_compile_test_src = files('scorpi_header_compile_test.c')

scorpi_header_compile_test_executable = executable(
  'scorpi_header_compile_test',
  scorpi_header_compile_test_src,
  include_directories: inc,
)

test('scorpi_header_compile_test', scorpi_header_compile_test_executable)

scorpi_image_backend_test_executable = executable(
  'scorpi_image_backend_test',
  files(
    'scorpi_image_backend_test.c',
    '../src/scorpi_image.c',
  ),
  dependencies: threads_dep,
  include_directories: [inc, include_directories('../src')],
)

test('scorpi_image_backend_test', scorpi_image_backend_test_executable)

scorpi_image_open_test_executable = executable(
  'scorpi_image_open_test',
  files(
    'scorpi_image_open_test.c',
    '../src/scorpi_image_chain.c',
    '../src/scorpi_image.c',
    '../src/scorpi_image_raw.c',
  ),
  dependencies: threads_dep,
  include_directories: [inc, include_directories('../src')],
)

test('scorpi_image_open_test', scorpi_image_open_test_executable)

scorpi_image_uri_test_executable = executable(
  'scorpi_image_uri_test',
  files('scorpi_image_uri_test.c', '../src/scorpi_image_uri.c'),
  include_directories: [inc, include_directories('../src')],
)

test('scorpi_image_uri_test', scorpi_image_uri_test_executable)

scorpi_image_raw_test_executable = executable(
  'scorpi_image_raw_test',
  files('scorpi_image_raw_test.c', '../src/scorpi_image_raw.c'),
  include_directories: [inc, include_directories('../src')],
)

test('scorpi_image_raw_test', scorpi_image_raw_test_executable)

scorpi_image_chain_test_executable = executable(
  'scorpi_image_chain_test',
  files(
    'scorpi_image_chain_test.c',
    '../src/scorpi_image_chain.c',
    '../src/scorpi_image.c',
    '../src/scorpi_image_raw.c',
  ),
  include_directories: [inc, include_directories('../src')],
)

test('scorpi_image_chain_test', scorpi_image_chain_test_executable)

scorpi_error_contract_test_src = files('scorpi_error_contract_test.c')

scorpi_error_contract_test_executable = executable(
  'scorpi_error_contract_test',
  scorpi_error_contract_test_src,
  dependencies: scorpi_kit_dep,
  include_directories: inc,
)

test('scorpi_error_contract_test', scorpi_error_contract_test_executable)

libscorpi_test_inc = include_directories('../src/libscorpi')

scorpi_vm_builder_test_src = files('scorpi_vm_builder_test.c')

scorpi_vm_builder_test_executable = executable(
  'scorpi_vm_builder_test',
  scorpi_vm_builder_test_src,
  dependencies: scorpi_kit_dep,
  include_directories: [inc, libscorpi_test_inc],
)

test('scorpi_vm_builder_test', scorpi_vm_builder_test_executable)

scorpi_device_builder_test_src = files('scorpi_device_builder_test.c')

scorpi_device_builder_test_executable = executable(
  'scorpi_device_builder_test',
  scorpi_device_builder_test_src,
  dependencies: scorpi_kit_dep,
  include_directories: [inc, libscorpi_test_inc],
)

test('scorpi_device_builder_test', scorpi_device_builder_test_executable)

scorpi_vm_add_device_test_src = files('scorpi_vm_add_device_test.c')

scorpi_vm_add_device_test_executable = executable(
  'scorpi_vm_add_device_test',
  scorpi_vm_add_device_test_src,
  dependencies: scorpi_kit_dep,
  include_directories: [inc, libscorpi_test_inc],
)

test('scorpi_vm_add_device_test', scorpi_vm_add_device_test_executable)

scorpi_parent_resolution_test_src = files('scorpi_parent_resolution_test.c')

scorpi_parent_resolution_test_executable = executable(
  'scorpi_parent_resolution_test',
  scorpi_parent_resolution_test_src,
  dependencies: scorpi_kit_dep,
  include_directories: [inc, libscorpi_test_inc],
)

test('scorpi_parent_resolution_test', scorpi_parent_resolution_test_executable)

scorpi_vm_normalization_test_src = files('scorpi_vm_normalization_test.c')

scorpi_vm_normalization_test_executable = executable(
  'scorpi_vm_normalization_test',
  scorpi_vm_normalization_test_src,
  dependencies: scorpi_kit_dep,
  include_directories: [inc, libscorpi_test_inc],
)

test('scorpi_vm_normalization_test', scorpi_vm_normalization_test_executable)

scorpi_yaml_loader_test_src = files('scorpi_yaml_loader_test.c')

scorpi_yaml_loader_test_executable = executable(
  'scorpi_yaml_loader_test',
  scorpi_yaml_loader_test_src,
  dependencies: scorpi_kit_dep,
  include_directories: [inc, libscorpi_test_inc],
)

test('scorpi_yaml_loader_test', scorpi_yaml_loader_test_executable)

scorpi_yaml_normalization_rules_test_src = files(
  'scorpi_yaml_normalization_rules_test.c'
)

scorpi_yaml_normalization_rules_test_executable = executable(
  'scorpi_yaml_normalization_rules_test',
  scorpi_yaml_normalization_rules_test_src,
  dependencies: scorpi_kit_dep,
  include_directories: [inc, libscorpi_test_inc],
)

test(
  'scorpi_yaml_normalization_rules_test',
  scorpi_yaml_normalization_rules_test_executable
)

scorpi_vm_validation_test_src = files('scorpi_vm_validation_test.c')

scorpi_vm_validation_test_executable = executable(
  'scorpi_vm_validation_test',
  scorpi_vm_validation_test_src,
  dependencies: scorpi_kit_dep,
  include_directories: [inc, libscorpi_test_inc],
)

test('scorpi_vm_validation_test', scorpi_vm_validation_test_executable)

scorpi_config_adapter_test_src = files('scorpi_config_adapter_test.c')

scorpi_config_adapter_test_executable = executable(
  'scorpi_config_adapter_test',
  scorpi_config_adapter_test_src,
  dependencies: scorpi_kit_dep,
  include_directories: [inc, libscorpi_test_inc],
)

test('scorpi_config_adapter_test', scorpi_config_adapter_test_executable)

scorpi_vm_lifecycle_test_src = files('scorpi_vm_lifecycle_test.c')

scorpi_vm_lifecycle_test_executable = executable(
  'scorpi_vm_lifecycle_test',
  scorpi_vm_lifecycle_test_src,
  dependencies: scorpi_kit_dep,
  include_directories: [inc, libscorpi_test_inc],
)

test('scorpi_vm_lifecycle_test', scorpi_vm_lifecycle_test_executable)

scorpi_cli_config_parity_test_src = files('scorpi_cli_config_parity_test.c')

scorpi_cli_config_parity_test_executable = executable(
  'scorpi_cli_config_parity_test',
  scorpi_cli_config_parity_test_src,
  dependencies: [scorpi_kit_dep, libnv_dep],
  include_directories: [inc, libscorpi_test_inc],
)

test('scorpi_cli_config_parity_test', scorpi_cli_config_parity_test_executable)

scorpi_yaml_api_parity_test_src = files('scorpi_yaml_api_parity_test.c')

scorpi_yaml_api_parity_test_executable = executable(
  'scorpi_yaml_api_parity_test',
  scorpi_yaml_api_parity_test_src,
  dependencies: scorpi_kit_dep,
  include_directories: [inc, libscorpi_test_inc],
)

test('scorpi_yaml_api_parity_test', scorpi_yaml_api_parity_test_executable)

scorpi_yaml_file_cli_test_src = files(
  'scorpi_yaml_file_cli_test.c',
  '../src/scorpi_cli.c',
)

scorpi_yaml_file_cli_test_executable = executable(
  'scorpi_yaml_file_cli_test',
  scorpi_yaml_file_cli_test_src,
  dependencies: scorpi_kit_dep,
  include_directories: [inc, libscorpi_test_inc],
)

test('scorpi_yaml_file_cli_test', scorpi_yaml_file_cli_test_executable)
