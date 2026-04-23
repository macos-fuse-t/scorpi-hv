/* Parent resolution coverage for the early scorpi_kit implementation. */

#include <assert.h>
#include <stddef.h>

#include "scorpi_internal.h"

int
main(void)
{
	const scorpi_device_t *parent;
	scorpi_device_t *bridge;
	scorpi_device_t *child;
	scorpi_device_t *orphan;
	scorpi_device_t *self;
	scorpi_vm_t *vm;

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	assert(vm != NULL);

	assert(scorpi_create_pci_device("pci-bridge", 3, &bridge) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(bridge, "id", "bridge0") ==
	    SCORPI_OK);
	assert(scorpi_vm_add_device(vm, bridge) == SCORPI_OK);

	assert(scorpi_create_pci_device("virtio-net", 5, &child) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(child, "id", "net0") == SCORPI_OK);
	assert(scorpi_device_set_prop_string(child, "parent", "bridge0") ==
	    SCORPI_OK);
	assert(scorpi_vm_add_device(vm, child) == SCORPI_OK);
	assert(scorpi_vm_resolve_parent(vm, child, &parent) == SCORPI_OK);
	assert(parent == bridge);

	assert(scorpi_create_usb_device("tablet", &orphan) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(orphan, "id", "tablet0") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(orphan, "parent", "missing0") ==
	    SCORPI_OK);
	assert(scorpi_vm_resolve_parent(vm, orphan, &parent) ==
	    SCORPI_ERR_INVALID_PARENT);
	scorpi_destroy_device(orphan);

	assert(scorpi_create_usb_device("keyboard", &self) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(self, "id", "kbd0") == SCORPI_OK);
	assert(scorpi_device_set_prop_string(self, "parent", "kbd0") ==
	    SCORPI_OK);
	assert(scorpi_vm_resolve_parent(vm, self, &parent) ==
	    SCORPI_ERR_INVALID_PARENT);
	scorpi_destroy_device(self);

	scorpi_destroy_vm(vm);
	return (0);
}
