/* VM builder coverage for the early scorpi_kit implementation. */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scorpi_internal.h"

int
main(void)
{
	const struct scorpi_prop *prop;
	scorpi_vm_t *vm;
	uint64_t ram_bytes;

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	assert(vm != NULL);

	assert(scorpi_vm_set_cpu(vm, 2) == SCORPI_OK);
	prop = scorpi_vm_find_prop(vm, "cpu.cores");
	assert(prop != NULL);
	assert(prop->kind == SCORPI_PROP_U64);
	assert(prop->value.u64 == 2);

	ram_bytes = 4ULL * 1024 * 1024 * 1024;
	assert(scorpi_vm_set_ram(vm, ram_bytes) == SCORPI_OK);
	prop = scorpi_vm_find_prop(vm, "memory.size");
	assert(prop != NULL);
	assert(prop->kind == SCORPI_PROP_U64);
	assert(prop->value.u64 == ram_bytes);

	assert(scorpi_vm_set_prop_bool(vm, "graphics.fb", true) == SCORPI_OK);
	prop = scorpi_vm_find_prop(vm, "graphics.fb");
	assert(prop != NULL);
	assert(prop->kind == SCORPI_PROP_BOOL);
	assert(prop->value.boolean == true);

	scorpi_destroy_vm(vm);
	scorpi_destroy_vm(NULL);
	return (0);
}
