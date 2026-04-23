/* Error-contract coverage for early scorpi_kit implementation. */

#include <assert.h>
#include <stddef.h>

#include "scorpi.h"

int
main(void)
{
	scorpi_vm_t vm;
	scorpi_device_t dev;
	int exit_code;

	assert(scorpi_create_vm(NULL) == SCORPI_ERR_INVALID_ARG);
	assert(scorpi_create_vm(&vm) == SCORPI_OK);

	assert(scorpi_create_pci_device(NULL, 1, &dev) == SCORPI_ERR_INVALID_ARG);
	assert(scorpi_create_usb_device("", &dev) == SCORPI_ERR_INVALID_ARG);
	assert(scorpi_load_vm_from_yaml(NULL, &vm) == SCORPI_ERR_INVALID_ARG);

	assert(scorpi_start_vm(vm) == SCORPI_ERR_UNSUPPORTED);
	assert(scorpi_wait_vm(vm, NULL) == SCORPI_ERR_INVALID_ARG);

	scorpi_destroy_vm(vm);

	exit_code = 0;
	(void)exit_code;
	return (0);
}
