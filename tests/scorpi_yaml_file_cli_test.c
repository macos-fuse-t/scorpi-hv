/* Verify YAML file loading matches direct YAML string loading. */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "scorpi_cli.h"
#include "scorpi_internal.h"

static void
write_all(int fd, const char *contents)
{
	size_t remaining;
	ssize_t written;

	remaining = strlen(contents);
	while (remaining != 0) {
		written = write(fd, contents, remaining);
		assert(written > 0);
		contents += written;
		remaining -= (size_t)written;
	}
}

static void
test_yaml_file_parity(void)
{
	static const char yaml[] =
	    "name: yaml-file-cli\n"
	    "cpu: 2\n"
	    "memory: 4G\n"
	    "devices:\n"
	    "  pci:\n"
	    "    - device: xhci\n"
	    "      slot: 1\n"
	    "      bus: 0\n"
	    "  lpc:\n"
	    "    - device: vm-control\n"
	    "      path: /tmp/scorpi-yaml-cli.sock\n";
	struct scorpi_normalized_vm *from_file_normalized;
	struct scorpi_normalized_vm *from_string_normalized;
	scorpi_vm_t from_file_vm;
	scorpi_vm_t from_string_vm;
	char path[] = "/tmp/scorpi-yaml-file-XXXXXX";
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	write_all(fd, yaml);
	assert(close(fd) == 0);

	assert(scorpi_load_vm_from_yaml(yaml, &from_string_vm) == SCORPI_OK);
	assert(scorpi_cli_load_vm_from_yaml_file(path, &from_file_vm) ==
	    SCORPI_OK);
	assert(scorpi_vm_normalize(from_string_vm, &from_string_normalized) ==
	    SCORPI_OK);
	assert(scorpi_vm_normalize(from_file_vm, &from_file_normalized) ==
	    SCORPI_OK);
	assert(scorpi_normalized_vm_equal(from_string_normalized,
	    from_file_normalized));

	scorpi_normalized_vm_destroy(from_string_normalized);
	scorpi_normalized_vm_destroy(from_file_normalized);
	scorpi_destroy_vm(from_string_vm);
	scorpi_destroy_vm(from_file_vm);
	assert(unlink(path) == 0);
}

static void
test_invalid_yaml_file(void)
{
	scorpi_vm_t vm;
	char path[] = "/tmp/scorpi-yaml-invalid-XXXXXX";
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	write_all(fd, "cpu: [1\n");
	assert(close(fd) == 0);

	assert(scorpi_cli_load_vm_from_yaml_file(path, &vm) ==
	    SCORPI_ERR_YAML_PARSE);
	assert(vm == NULL);
	assert(unlink(path) == 0);
}

static void
test_cpu_timer_options(void)
{
	static const char *const yaml[] = {
	    "cpu:\n  cores: 4\n  hyperv: true\n  profile-timer: true\n"
	    "  nested-virt: false\nmemory: 4G\n",
	    "cpu:\n  cores: 4\n  hyperv: false\n  profile-timer: false\n"
	    "  nested-virt: false\nmemory: 4G\n",
	    "cpu:\n  cores: 4\nmemory: 4G\n",
	};
	static const char *const expected[] = {"true", "false", NULL};

	for (size_t i = 0; i < sizeof(yaml) / sizeof(yaml[0]); i++) {
		struct scorpi_normalized_vm *normalized, *roundtrip;
		scorpi_vm_t vm, from_config;
		nvlist_t *config;

		assert(scorpi_load_vm_from_yaml(yaml[i], &vm) == SCORPI_OK);
		assert(scorpi_vm_normalize(vm, &normalized) == SCORPI_OK);
		assert(scorpi_vm_to_config(normalized, &config) == SCORPI_OK);
		const char *hyperv = scorpi_config_get_value(config, "cpu.hyperv");
		const char *timer = scorpi_config_get_value(config, "cpu.profile-timer");
		if (expected[i] == NULL) {
			assert(hyperv == NULL && timer == NULL);
		} else {
			assert(hyperv != NULL && strcmp(hyperv, expected[i]) == 0);
			assert(timer != NULL && strcmp(timer, expected[i]) == 0);
		}
		/* CLI imports must preserve both explicit true and explicit false. */
		assert(scorpi_load_vm_from_config_tree(config, &from_config) == SCORPI_OK);
		assert(scorpi_vm_normalize(from_config, &roundtrip) == SCORPI_OK);
		assert(scorpi_normalized_vm_equal(normalized, roundtrip));
		scorpi_normalized_vm_destroy(roundtrip);
		scorpi_destroy_vm(from_config);
		scorpi_config_destroy(config);
		scorpi_normalized_vm_destroy(normalized);
		scorpi_destroy_vm(vm);
	}
}

static void
test_missing_yaml_file(void)
{
	scorpi_vm_t vm;

	assert(scorpi_cli_load_vm_from_yaml_file(
	    "/tmp/scorpi-yaml-missing-file.yaml", &vm) == SCORPI_ERR_RUNTIME);
	assert(vm == NULL);
}

int
main(void)
{
	test_yaml_file_parity();
	test_cpu_timer_options();
	test_invalid_yaml_file();
	test_missing_yaml_file();
	return (0);
}
