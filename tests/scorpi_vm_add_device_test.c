/* VM device attachment coverage for the early scorpi_kit implementation. */

#include <assert.h>
#include <stddef.h>

#include "scorpi_internal.h"

int
main(void)
{
	scorpi_device_t *dev;
	scorpi_device_t *dup;
	scorpi_vm_t *vm;

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	assert(vm != NULL);

	assert(scorpi_create_pci_device("virtio-net", 5, &dev) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "id", "net0") == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dev) == SCORPI_OK);
	assert(vm->devices == dev);
	assert(dev->attached == true);

	assert(scorpi_vm_add_device(vm, dev) == SCORPI_ERR_INVALID_ARG);

	assert(scorpi_create_usb_device("tablet", &dup) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(dup, "id", "net0") == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dup) == SCORPI_ERR_DUPLICATE_ID);
	assert(dup->attached == false);
	scorpi_destroy_device(dup);

	scorpi_destroy_vm(vm);
	return (0);
}
