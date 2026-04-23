/* Semantic validation coverage for the early scorpi_kit implementation. */

#include <assert.h>
#include <stddef.h>

#include "scorpi_internal.h"

static void
set_required_vm_props(scorpi_vm_t vm)
{
	assert(scorpi_vm_set_cpu(vm, 2) == SCORPI_OK);
	assert(scorpi_vm_set_ram(vm, 4ULL * 1024 * 1024 * 1024) == SCORPI_OK);
}

static scorpi_device_t
add_pci_device(scorpi_vm_t vm, const char *device, uint64_t slot,
    const char *id)
{
	scorpi_device_t dev;

	assert(scorpi_create_pci_device(device, slot, &dev) == SCORPI_OK);
	if (id != NULL)
		assert(scorpi_device_set_prop_string(dev, "id", id) == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dev) == SCORPI_OK);
	return (dev);
}

static scorpi_device_t
add_usb_device(scorpi_vm_t vm, const char *device)
{
	scorpi_device_t dev;

	assert(scorpi_create_usb_device(device, &dev) == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dev) == SCORPI_OK);
	return (dev);
}

static void
add_valid_lpc_devices(scorpi_vm_t vm)
{
	scorpi_device_t dev;

	assert(scorpi_create_lpc_device("vm-control", &dev) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "path", "/tmp/vm.sock") ==
	    SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dev) == SCORPI_OK);

	assert(scorpi_create_lpc_device("tpm", &dev) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "type", "swtpm") == SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "path",
	    "/tmp/swtpm.sock") == SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "version", "2.0") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "intf", "tis") == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dev) == SCORPI_OK);
}

static void
test_missing_required_props(void)
{
	scorpi_vm_t vm;

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	assert(scorpi_vm_validate(vm) == SCORPI_ERR_VALIDATION);
	assert(scorpi_start_vm(vm) == SCORPI_ERR_VALIDATION);
	scorpi_destroy_vm(vm);
}

static void
test_invalid_cpu_and_ram(void)
{
	scorpi_vm_t vm;

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	set_required_vm_props(vm);
	assert(scorpi_vm_set_prop_u64(vm, "cpu.cores", 0) == SCORPI_OK);
	assert(scorpi_vm_validate(vm) == SCORPI_ERR_VALIDATION);
	scorpi_destroy_vm(vm);

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	set_required_vm_props(vm);
	assert(scorpi_vm_set_prop_u64(vm, "memory.size", 0) == SCORPI_OK);
	assert(scorpi_vm_validate(vm) == SCORPI_ERR_VALIDATION);
	scorpi_destroy_vm(vm);
}

static void
test_duplicate_ids(void)
{
	scorpi_device_t bridge;
	scorpi_device_t net;
	scorpi_vm_t vm;

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	set_required_vm_props(vm);
	bridge = add_pci_device(vm, "pci-bridge", 3, "bridge0");
	net = add_pci_device(vm, "virtio-net", 5, "net0");

	assert(scorpi_device_set_prop_string(net, "id", "bridge0") == SCORPI_OK);
	assert(bridge != NULL);
	assert(scorpi_vm_validate(vm) == SCORPI_ERR_DUPLICATE_ID);
	scorpi_destroy_vm(vm);
}

static void
test_invalid_parent(void)
{
	scorpi_device_t net;
	scorpi_vm_t vm;

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	set_required_vm_props(vm);
	net = add_pci_device(vm, "virtio-net", 5, "net0");
	assert(scorpi_device_set_prop_string(net, "parent", "missing0") ==
	    SCORPI_OK);
	assert(scorpi_vm_validate(vm) == SCORPI_ERR_INVALID_PARENT);
	scorpi_destroy_vm(vm);
}

static void
test_duplicate_pci_slots(void)
{
	scorpi_vm_t vm;

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	set_required_vm_props(vm);
	add_pci_device(vm, "pci-bridge", 3, "bridge0");
	add_pci_device(vm, "virtio-net", 3, "net0");
	assert(scorpi_vm_validate(vm) == SCORPI_ERR_VALIDATION);
	scorpi_destroy_vm(vm);
}

static void
test_missing_usb_parent_when_required(void)
{
	scorpi_vm_t vm;

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	set_required_vm_props(vm);
	add_pci_device(vm, "xhci", 1, "xhci0");
	add_pci_device(vm, "xhci", 2, "xhci1");
	add_usb_device(vm, "tablet");
	assert(scorpi_vm_validate(vm) == SCORPI_ERR_VALIDATION);
	scorpi_destroy_vm(vm);
}

static void
test_invalid_lpc_configs(void)
{
	scorpi_device_t dev;
	scorpi_vm_t vm;

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	set_required_vm_props(vm);
	assert(scorpi_create_lpc_device("vm-control", &dev) == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dev) == SCORPI_OK);
	assert(scorpi_vm_validate(vm) == SCORPI_ERR_VALIDATION);
	scorpi_destroy_vm(vm);

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	set_required_vm_props(vm);
	assert(scorpi_create_lpc_device("tpm", &dev) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "type", "swtpm") == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dev) == SCORPI_OK);
	assert(scorpi_vm_validate(vm) == SCORPI_ERR_VALIDATION);
	scorpi_destroy_vm(vm);
}

static void
test_valid_configuration(void)
{
	scorpi_vm_t vm;

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	set_required_vm_props(vm);
	add_pci_device(vm, "xhci", 1, "xhci0");
	add_usb_device(vm, "tablet");
	add_valid_lpc_devices(vm);

	assert(scorpi_vm_validate(vm) == SCORPI_OK);
	assert(scorpi_start_vm(vm) == SCORPI_ERR_UNSUPPORTED);
	scorpi_destroy_vm(vm);
}

int
main(void)
{
	test_missing_required_props();
	test_invalid_cpu_and_ram();
	test_duplicate_ids();
	test_invalid_parent();
	test_duplicate_pci_slots();
	test_missing_usb_parent_when_required();
	test_invalid_lpc_configs();
	test_valid_configuration();
	return (0);
}
