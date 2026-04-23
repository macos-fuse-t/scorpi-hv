/* Ensure CLI config trees and API builders converge on the same VM model. */

#include <assert.h>

#include "nv.h"
#include "scorpi_internal.h"

static nvlist_t *
add_child_node(nvlist_t *parent, const char *name)
{
	nvlist_t *child;

	child = nvlist_create(0);
	assert(child != NULL);
	nvlist_move_nvlist(parent, name, child);
	return (child);
}

static void
build_cli_config_tree(nvlist_t **out_config)
{
	nvlist_t *bus0;
	nvlist_t *config;
	nvlist_t *func0;
	nvlist_t *memory;
	nvlist_t *port0;
	nvlist_t *port1;
	nvlist_t *port_root;
	nvlist_t *pci;
	nvlist_t *slot1;
	nvlist_t *slot2;
	nvlist_t *usb;
	nvlist_t *usb_slot1;

	config = nvlist_create(0);
	assert(config != NULL);

	nvlist_add_string(config, "name", "cli-parity");
	nvlist_add_string(config, "cpus", "2");
	nvlist_add_string(config, "comm_sock", "/tmp/scorpi-cli.sock");
	nvlist_add_string(config, "bootrom", "/tmp/cli-parity.efi");
	nvlist_add_string(config, "bootvars", "/tmp/cli-parity.vars");
	nvlist_add_string(config, "console", "stdio");

	memory = add_child_node(config, "memory");
	nvlist_add_string(memory, "size", "4G");

	pci = add_child_node(config, "pci");
	bus0 = add_child_node(pci, "0");
	slot1 = add_child_node(bus0, "1");
	func0 = add_child_node(slot1, "0");
	nvlist_add_string(func0, "device", "xhci");
	nvlist_add_string(func0, "bus", "0");

	slot2 = add_child_node(bus0, "2");
	func0 = add_child_node(slot2, "0");
	nvlist_add_string(func0, "device", "ahci");
	port_root = add_child_node(func0, "port");
	port0 = add_child_node(port_root, "0");
	nvlist_add_string(port0, "type", "hd");
	nvlist_add_string(port0, "path", "/tmp/cli-parity.img");
	port1 = add_child_node(port_root, "1");
	nvlist_add_string(port1, "type", "cd");
	nvlist_add_string(port1, "path", "/tmp/cli-parity.iso");
	nvlist_add_string(port1, "ro", "true");

	usb = add_child_node(config, "usb");
	bus0 = add_child_node(usb, "0");
	usb_slot1 = add_child_node(bus0, "1");
	nvlist_add_string(usb_slot1, "device", "tablet");

	*out_config = config;
}

static void
build_api_vm(scorpi_vm_t *out_vm)
{
	scorpi_device_t dev;
	scorpi_vm_t vm;

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	assert(scorpi_vm_set_prop_string(vm, "name", "cli-parity") == SCORPI_OK);
	assert(scorpi_vm_set_cpu(vm, 2) == SCORPI_OK);
	assert(scorpi_vm_set_ram(vm, 4ULL * 1024 * 1024 * 1024) == SCORPI_OK);
	assert(scorpi_vm_set_prop_string(vm, "bootrom",
	    "/tmp/cli-parity.efi") == SCORPI_OK);
	assert(scorpi_vm_set_prop_string(vm, "bootvars",
	    "/tmp/cli-parity.vars") == SCORPI_OK);
	assert(scorpi_vm_set_prop_string(vm, "console", "stdio") == SCORPI_OK);

	assert(scorpi_create_pci_device("xhci", 1, &dev) == SCORPI_OK);
	assert(scorpi_device_set_prop_u64(dev, "bus", 0) == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dev) == SCORPI_OK);

	assert(scorpi_create_pci_device("ahci", 2, &dev) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "port.0.path",
	    "/tmp/cli-parity.img") == SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "port.0.type", "hd") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "port.1.path",
	    "/tmp/cli-parity.iso") == SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "port.1.type", "cd") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_bool(dev, "port.1.ro", true) == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dev) == SCORPI_OK);

	assert(scorpi_create_usb_device("tablet", &dev) == SCORPI_OK);
	assert(scorpi_device_set_prop_u64(dev, "port", 1) == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dev) == SCORPI_OK);

	assert(scorpi_create_lpc_device("vm-control", &dev) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "path",
	    "/tmp/scorpi-cli.sock") == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dev) == SCORPI_OK);

	*out_vm = vm;
}

int
main(void)
{
	nvlist_t *config;
	struct scorpi_normalized_vm *cli_normalized;
	struct scorpi_normalized_vm *api_normalized;
	scorpi_vm_t cli_vm;
	scorpi_vm_t api_vm;

	build_cli_config_tree(&config);
	build_api_vm(&api_vm);

	assert(scorpi_load_vm_from_config_tree(config, &cli_vm) == SCORPI_OK);
	assert(scorpi_vm_normalize(cli_vm, &cli_normalized) == SCORPI_OK);
	assert(scorpi_vm_normalize(api_vm, &api_normalized) == SCORPI_OK);
	assert(scorpi_normalized_vm_equal(cli_normalized, api_normalized));

	scorpi_normalized_vm_destroy(cli_normalized);
	scorpi_normalized_vm_destroy(api_normalized);
	scorpi_destroy_vm(cli_vm);
	scorpi_destroy_vm(api_vm);
	nvlist_destroy(config);
	return (0);
}
