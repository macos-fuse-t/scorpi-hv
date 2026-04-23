/* Config adapter coverage for the early scorpi_kit implementation. */

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "scorpi_internal.h"

static void
build_vm_variant_a(scorpi_vm_t *out_vm)
{
	scorpi_device_t ahci;
	scorpi_device_t tpm;
	scorpi_device_t usb;
	scorpi_device_t vm_control;
	scorpi_device_t xhci;
	scorpi_vm_t vm;

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	assert(scorpi_vm_set_cpu(vm, 2) == SCORPI_OK);
	assert(scorpi_vm_set_ram(vm, 4ULL * 1024 * 1024 * 1024) == SCORPI_OK);
	assert(scorpi_vm_set_prop_bool(vm, "memory.wired", true) == SCORPI_OK);

	assert(scorpi_create_pci_device("xhci", 1, &xhci) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(xhci, "id", "xhci0") == SCORPI_OK);
	assert(scorpi_device_set_prop_u64(xhci, "bus", 0) == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, xhci) == SCORPI_OK);

	assert(scorpi_create_pci_device("ahci", 2, &ahci) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(ahci, "port.0.type", "hd") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(ahci, "port.0.path",
	    "/tmp/disk.img") == SCORPI_OK);
	assert(scorpi_device_set_prop_string(ahci, "port.1.type", "cd") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(ahci, "port.1.path",
	    "/tmp/boot.iso") == SCORPI_OK);
	assert(scorpi_device_set_prop_bool(ahci, "port.1.ro", true) ==
	    SCORPI_OK);
	assert(scorpi_vm_add_device(vm, ahci) == SCORPI_OK);

	assert(scorpi_create_usb_device("tablet", &usb) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(usb, "parent", "xhci0") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_u64(usb, "port", 7) == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, usb) == SCORPI_OK);

	assert(scorpi_create_lpc_device("vm-control", &vm_control) ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(vm_control, "path",
	    "/tmp/vm.sock") == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, vm_control) == SCORPI_OK);

	assert(scorpi_create_lpc_device("tpm", &tpm) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(tpm, "type", "swtpm") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(tpm, "path", "/tmp/tpm.sock") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(tpm, "version", "2.0") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(tpm, "intf", "tis") == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, tpm) == SCORPI_OK);

	*out_vm = vm;
}

static void
build_vm_variant_b(scorpi_vm_t *out_vm)
{
	scorpi_device_t ahci;
	scorpi_device_t tpm;
	scorpi_device_t usb;
	scorpi_device_t vm_control;
	scorpi_device_t xhci;
	scorpi_vm_t vm;

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	assert(scorpi_vm_set_prop_bool(vm, "memory.wired", true) == SCORPI_OK);
	assert(scorpi_vm_set_ram(vm, 4ULL * 1024 * 1024 * 1024) == SCORPI_OK);
	assert(scorpi_vm_set_cpu(vm, 2) == SCORPI_OK);

	assert(scorpi_create_lpc_device("tpm", &tpm) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(tpm, "intf", "tis") == SCORPI_OK);
	assert(scorpi_device_set_prop_string(tpm, "version", "2.0") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(tpm, "path", "/tmp/tpm.sock") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(tpm, "type", "swtpm") ==
	    SCORPI_OK);
	assert(scorpi_vm_add_device(vm, tpm) == SCORPI_OK);

	assert(scorpi_create_lpc_device("vm-control", &vm_control) ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(vm_control, "path",
	    "/tmp/vm.sock") == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, vm_control) == SCORPI_OK);

	assert(scorpi_create_pci_device("ahci", 2, &ahci) == SCORPI_OK);
	assert(scorpi_device_set_prop_bool(ahci, "port.1.ro", true) ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(ahci, "port.1.path",
	    "/tmp/boot.iso") == SCORPI_OK);
	assert(scorpi_device_set_prop_string(ahci, "port.1.type", "cd") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(ahci, "port.0.path",
	    "/tmp/disk.img") == SCORPI_OK);
	assert(scorpi_device_set_prop_string(ahci, "port.0.type", "hd") ==
	    SCORPI_OK);
	assert(scorpi_vm_add_device(vm, ahci) == SCORPI_OK);

	assert(scorpi_create_usb_device("tablet", &usb) == SCORPI_OK);
	assert(scorpi_device_set_prop_u64(usb, "port", 7) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(usb, "parent", "xhci0") ==
	    SCORPI_OK);
	assert(scorpi_vm_add_device(vm, usb) == SCORPI_OK);

	assert(scorpi_create_pci_device("xhci", 1, &xhci) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(xhci, "id", "xhci0") == SCORPI_OK);
	assert(scorpi_device_set_prop_u64(xhci, "bus", 0) == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, xhci) == SCORPI_OK);

	*out_vm = vm;
}

int
main(void)
{
	struct scorpi_normalized_vm *norm_a;
	struct scorpi_normalized_vm *norm_b;
	nvlist_t *config_a;
	nvlist_t *config_b;
	scorpi_vm_t vm_a;
	scorpi_vm_t vm_b;

	build_vm_variant_a(&vm_a);
	build_vm_variant_b(&vm_b);

	assert(scorpi_vm_normalize(vm_a, &norm_a) == SCORPI_OK);
	assert(scorpi_vm_normalize(vm_b, &norm_b) == SCORPI_OK);
	assert(scorpi_vm_to_config(norm_a, &config_a) == SCORPI_OK);
	assert(scorpi_vm_to_config(norm_b, &config_b) == SCORPI_OK);

	assert(strcmp(scorpi_config_get_value(config_a, "cpus"), "2") == 0);
	assert(strcmp(scorpi_config_get_value(config_a, "cores"), "2") == 0);
	assert(strcmp(scorpi_config_get_value(config_a, "sockets"), "1") == 0);
	assert(strcmp(scorpi_config_get_value(config_a, "threads"), "1") == 0);
	assert(strcmp(scorpi_config_get_value(config_a, "memory.size"),
	    "4294967296") == 0);
	assert(strcmp(scorpi_config_get_value(config_a, "memory.wired"),
	    "true") == 0);

	assert(strcmp(scorpi_config_get_value(config_a, "pci.0.1.0.device"),
	    "xhci") == 0);
	assert(strcmp(scorpi_config_get_value(config_a, "pci.0.1.0.bus"),
	    "0") == 0);
	assert(strcmp(scorpi_config_get_value(config_a, "pci.0.2.0.device"),
	    "ahci") == 0);
	assert(strcmp(scorpi_config_get_value(config_a, "pci.0.2.0.port.0.type"),
	    "hd") == 0);
	assert(strcmp(scorpi_config_get_value(config_a, "pci.0.2.0.port.0.path"),
	    "/tmp/disk.img") == 0);
	assert(strcmp(scorpi_config_get_value(config_a, "pci.0.2.0.port.1.type"),
	    "cd") == 0);
	assert(strcmp(scorpi_config_get_value(config_a, "pci.0.2.0.port.1.path"),
	    "/tmp/boot.iso") == 0);
	assert(strcmp(scorpi_config_get_value(config_a, "pci.0.2.0.port.1.ro"),
	    "true") == 0);
	assert(strcmp(scorpi_config_get_value(config_a, "usb.0.7.device"),
	    "tablet") == 0);
	assert(strcmp(scorpi_config_get_value(config_a, "comm_sock"),
	    "/tmp/vm.sock") == 0);
	assert(strcmp(scorpi_config_get_value(config_a, "tpm.type"),
	    "swtpm") == 0);
	assert(strcmp(scorpi_config_get_value(config_a, "tpm.path"),
	    "/tmp/tpm.sock") == 0);
	assert(strcmp(scorpi_config_get_value(config_a, "tpm.version"),
	    "2.0") == 0);
	assert(strcmp(scorpi_config_get_value(config_a, "tpm.intf"),
	    "tis") == 0);

	assert(scorpi_config_equal(config_a, config_b) == true);

	scorpi_config_destroy(config_b);
	scorpi_config_destroy(config_a);
	scorpi_normalized_vm_destroy(norm_b);
	scorpi_normalized_vm_destroy(norm_a);
	scorpi_destroy_vm(vm_b);
	scorpi_destroy_vm(vm_a);
	return (0);
}
