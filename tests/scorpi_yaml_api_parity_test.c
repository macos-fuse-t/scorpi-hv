/* Realistic parity coverage between YAML and the builder API. */

#include <assert.h>

#include "scorpi_internal.h"

static void
build_api_vm(scorpi_vm_t *out_vm)
{
	scorpi_device_t dev;
	scorpi_vm_t vm;

	assert(scorpi_create_vm(&vm) == SCORPI_OK);
	assert(scorpi_vm_set_prop_string(vm, "name", "yaml-api-parity") ==
	    SCORPI_OK);
	assert(scorpi_vm_set_cpu(vm, 2) == SCORPI_OK);
	assert(scorpi_vm_set_ram(vm, 4ULL * 1024 * 1024 * 1024) == SCORPI_OK);

	assert(scorpi_create_pci_device("hostbridge", 0, &dev) == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dev) == SCORPI_OK);

	assert(scorpi_create_pci_device("xhci", 1, &dev) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "id", "xhci0") == SCORPI_OK);
	assert(scorpi_device_set_prop_u64(dev, "bus", 0) == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dev) == SCORPI_OK);

	assert(scorpi_create_pci_device("virtio-blk", 2, &dev) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "path", "/tmp/disk.img") ==
	    SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dev) == SCORPI_OK);

	assert(scorpi_create_pci_device("ahci", 3, &dev) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "port.0.type", "hd") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "port.0.path", "/tmp/boot.img") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "port.1.type", "cd") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "port.1.path", "/tmp/boot.iso") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_bool(dev, "port.1.ro", true) == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dev) == SCORPI_OK);

	assert(scorpi_create_pci_device("virtio-net", 4, &dev) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "mac", "52:54:00:12:34:56") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "backend", "slirp") ==
	    SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dev) == SCORPI_OK);

	assert(scorpi_create_lpc_device("vm-control", &dev) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "path", "/tmp/vm.sock") ==
	    SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dev) == SCORPI_OK);

	assert(scorpi_create_lpc_device("tpm", &dev) == SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "type", "swtpm") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "path", "/tmp/swtpm.sock") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "version", "2.0") ==
	    SCORPI_OK);
	assert(scorpi_device_set_prop_string(dev, "intf", "crb") == SCORPI_OK);
	assert(scorpi_vm_add_device(vm, dev) == SCORPI_OK);

	*out_vm = vm;
}

int
main(void)
{
	static const char yaml[] =
	    "name: yaml-api-parity\n"
	    "cpu: 2\n"
	    "memory: 4G\n"
	    "devices:\n"
	    "  pci:\n"
	    "    - device: hostbridge\n"
	    "      slot: 0\n"
	    "    - device: xhci\n"
	    "      slot: 1\n"
	    "      id: xhci0\n"
	    "      bus: 0\n"
	    "    - device: virtio-blk\n"
	    "      slot: 2\n"
	    "      path: /tmp/disk.img\n"
	    "    - device: ahci\n"
	    "      slot: 3\n"
	    "      port.0.type: hd\n"
	    "      port.0.path: /tmp/boot.img\n"
	    "      port.1.type: cd\n"
	    "      port.1.path: /tmp/boot.iso\n"
	    "      port.1.ro: true\n"
	    "    - device: virtio-net\n"
	    "      slot: 4\n"
	    "      mac: 52:54:00:12:34:56\n"
	    "      backend: slirp\n"
	    "  lpc:\n"
	    "    - device: vm-control\n"
	    "      path: /tmp/vm.sock\n"
	    "    - device: tpm\n"
	    "      type: swtpm\n"
	    "      path: /tmp/swtpm.sock\n"
	    "      version: 2.0\n"
	    "      intf: crb\n";
	struct scorpi_normalized_vm *yaml_normalized;
	struct scorpi_normalized_vm *api_normalized;
	scorpi_vm_t yaml_vm;
	scorpi_vm_t api_vm;

	build_api_vm(&api_vm);
	assert(scorpi_load_vm_from_yaml(yaml, &yaml_vm) == SCORPI_OK);
	assert(scorpi_vm_normalize(api_vm, &api_normalized) == SCORPI_OK);
	assert(scorpi_vm_normalize(yaml_vm, &yaml_normalized) == SCORPI_OK);
	assert(scorpi_normalized_vm_equal(api_normalized, yaml_normalized));

	scorpi_normalized_vm_destroy(api_normalized);
	scorpi_normalized_vm_destroy(yaml_normalized);
	scorpi_destroy_vm(api_vm);
	scorpi_destroy_vm(yaml_vm);
	return (0);
}
