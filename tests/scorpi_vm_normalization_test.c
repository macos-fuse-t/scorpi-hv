/* Normalization coverage for the early scorpi_kit implementation. */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "scorpi_internal.h"

static void
build_vm_variant_a(scorpi_vm_t *out_vm)
{
	scorpi_device_t bridge;
	scorpi_device_t net;
	scorpi_vm_t vm;

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	assert(scorpi_vm_set_prop_bool(vm, "graphics.fb", true) == SCORPI_OK);
	assert(scorpi_vm_set_ram(vm, 4ULL * 1024 * 1024 * 1024) == SCORPI_OK);
	assert(scorpi_vm_set_cpu(vm, 2) == SCORPI_OK);

	assert(scorpi_create_pci_device("virtio-net", 5, &net) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(net, "id", "net0") == SCORPI_OK);
	assert(scorpi_device_set_prop_string(net, "parent", "bridge0") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(net, "mac",
	    "52:54:00:12:34:56") == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, net) == SCORPI_OK);

	assert(scorpi_create_pci_device("pci-bridge", 3, &bridge) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(bridge, "id", "bridge0") ==
	    SCORPI_OK);
	assert(scorpi_vm_add_device(vm, bridge) == SCORPI_OK);

	*out_vm = vm;
}

static void
build_vm_variant_b(scorpi_vm_t *out_vm)
{
	scorpi_device_t bridge;
	scorpi_device_t net;
	scorpi_vm_t vm;

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	assert(scorpi_vm_set_cpu(vm, 2) == SCORPI_OK);
	assert(scorpi_vm_set_prop_bool(vm, "graphics.fb", true) == SCORPI_OK);
	assert(scorpi_vm_set_ram(vm, 4ULL * 1024 * 1024 * 1024) == SCORPI_OK);

	assert(scorpi_create_pci_device("pci-bridge", 3, &bridge) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(bridge, "id", "bridge0") ==
	    SCORPI_OK);
	assert(scorpi_vm_add_device(vm, bridge) == SCORPI_OK);

	assert(scorpi_create_pci_device("virtio-net", 5, &net) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(net, "mac",
	    "52:54:00:12:34:56") == SCORPI_OK);
	assert(scorpi_device_set_prop_string(net, "parent", "bridge0") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(net, "id", "net0") == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, net) == SCORPI_OK);

	*out_vm = vm;
}

int
main(void)
{
	const struct scorpi_normalized_prop *prop;
	struct scorpi_normalized_vm *norm_a1;
	struct scorpi_normalized_vm *norm_a2;
	struct scorpi_normalized_vm *norm_b;
	scorpi_vm_t vm_a;
	scorpi_vm_t vm_b;

	build_vm_variant_a(&vm_a);
	build_vm_variant_b(&vm_b);

	assert(scorpi_vm_normalize(vm_a, &norm_a1) == SCORPI_OK);
	assert(scorpi_vm_normalize(vm_a, &norm_a2) == SCORPI_OK);
	assert(scorpi_vm_normalize(vm_b, &norm_b) == SCORPI_OK);

	assert(scorpi_normalized_vm_equal(norm_a1, norm_a2) == true);
	assert(scorpi_normalized_vm_equal(norm_a1, norm_b) == true);

	prop = scorpi_normalized_vm_find_prop(norm_a1, "cpu.cores");
	assert(prop != NULL);
	assert(prop->kind == SCORPI_PROP_U64);
	assert(prop->value.u64 == 2);

	prop = scorpi_normalized_vm_find_prop(norm_a1, "memory.size");
	assert(prop != NULL);
	assert(prop->kind == SCORPI_PROP_U64);
	assert(prop->value.u64 == 4ULL * 1024 * 1024 * 1024);

	assert(scorpi_normalized_vm_find_prop(norm_a1, "cpu") == NULL);
	assert(scorpi_normalized_vm_find_prop(norm_a1, "memory") == NULL);

	assert(norm_a1->device_count == 2);
	assert(norm_a1->devices[0].id != NULL);
	assert(norm_a1->devices[1].id != NULL);
	assert(strcmp(norm_a1->devices[0].id, "bridge0") == 0);
	assert(strcmp(norm_a1->devices[1].id, "net0") == 0);

	scorpi_normalized_vm_destroy(norm_b);
	scorpi_normalized_vm_destroy(norm_a2);
	scorpi_normalized_vm_destroy(norm_a1);
	scorpi_destroy_vm(vm_b);
	scorpi_destroy_vm(vm_a);
	return (0);
}
