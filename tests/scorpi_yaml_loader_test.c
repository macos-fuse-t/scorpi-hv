/* YAML loader coverage for the early scorpi_kit implementation. */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "scorpi_internal.h"

int
main(void)
{
	static const char valid_yaml[] =
	    "cpu: 2\n"
	    "memory: 4G\n"
	    "graphics.fb: true\n"
	    "devices:\n"
	    "  pci:\n"
	    "    - device: virtio-net\n"
	    "      id: net0\n"
	    "      parent: bridge0\n"
	    "      slot: 5\n"
	    "      mac: 52:54:00:12:34:56\n"
	    "    - device: pci-bridge\n"
	    "      id: bridge0\n";
	static const char invalid_yaml[] = "cpu: [1\n";
	const struct scorpi_normalized_prop *prop;
	struct scorpi_normalized_vm *normalized_vm;
	scorpi_vm_t vm;

	assert(scorpi_load_vm_from_yaml(valid_yaml, &vm) == SCORPI_OK);
	assert(vm != NULL);
	assert(scorpi_vm_normalize(vm, &normalized_vm) == SCORPI_OK);

	prop = scorpi_normalized_vm_find_prop(normalized_vm, "cpu.cores");
	assert(prop != NULL);
	assert(prop->kind == SCORPI_PROP_U64);
	assert(prop->value.u64 == 2);

	prop = scorpi_normalized_vm_find_prop(normalized_vm, "memory.size");
	assert(prop != NULL);
	assert(prop->kind == SCORPI_PROP_U64);
	assert(prop->value.u64 == 4ULL * 1024 * 1024 * 1024);

	prop = scorpi_normalized_vm_find_prop(normalized_vm, "graphics.fb");
	assert(prop != NULL);
	assert(prop->kind == SCORPI_PROP_BOOL);
	assert(prop->value.boolean == true);

	assert(normalized_vm->device_count == 2);
	assert(strcmp(normalized_vm->devices[0].id, "bridge0") == 0);
	assert(strcmp(normalized_vm->devices[1].id, "net0") == 0);

	scorpi_normalized_vm_destroy(normalized_vm);
	scorpi_destroy_vm(vm);

	assert(scorpi_load_vm_from_yaml(invalid_yaml, &vm) ==
	    SCORPI_ERR_YAML_PARSE);
	assert(vm == NULL);
	return (0);
}
