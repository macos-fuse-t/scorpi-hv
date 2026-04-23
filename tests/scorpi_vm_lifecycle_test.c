/* Lifecycle coverage for the early scorpi_kit implementation. */

#include <assert.h>
#include <stddef.h>

#include "scorpi_internal.h"

static void
build_runtime_attempt_vm(scorpi_vm_t *out_vm)
{
	scorpi_device_t vm_control;
	scorpi_device_t xhci;
	scorpi_vm_t vm;

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	assert(scorpi_vm_set_cpu(vm, 2) == SCORPI_OK);
	assert(scorpi_vm_set_ram(vm, 4ULL * 1024 * 1024 * 1024) == SCORPI_OK);

	assert(scorpi_create_pci_device("xhci", 1, &xhci) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(xhci, "id", "xhci0") == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, xhci) == SCORPI_OK);

	assert(scorpi_create_lpc_device("vm-control", &vm_control) ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(vm_control, "path",
	    "/tmp/scorpi-test.sock") == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, vm_control) == SCORPI_OK);

	*out_vm = vm;
}

int
main(void)
{
	int exit_code;
	scorpi_vm_t vm;

	build_runtime_attempt_vm(&vm);

	exit_code = scorpi_start_vm(vm);
	assert(exit_code != 0);
	assert(scorpi_start_vm(vm) == -SCORPI_ERR_INVALID_ARG);

	scorpi_destroy_vm(vm);
	return (0);
}
