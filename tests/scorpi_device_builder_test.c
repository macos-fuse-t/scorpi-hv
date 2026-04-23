/* Device builder coverage for the early scorpi_kit implementation. */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "scorpi_internal.h"

int
main(void)
{
	const struct scorpi_prop *prop;
	scorpi_device_t auto_pci;
	scorpi_device_t lpc;
	scorpi_device_t pci;
	scorpi_device_t usb;

	assert(scorpi_create_pci_device("virtio-net", 5, &pci) == SCORPI_OK);
	assert(pci != NULL);
	assert(pci->slot == 5);

	assert(scorpi_device_set_prop_string(pci, "mac", "52:54:00:12:34:56") ==
	    SCORPI_OK);
	prop = scorpi_device_find_prop(pci, "mac");
	assert(prop != NULL);
	assert(prop->kind == SCORPI_PROP_STRING);

	assert(scorpi_device_set_prop_u64(pci, "queue_count", 4) == SCORPI_OK);
	prop = scorpi_device_find_prop(pci, "queue_count");
	assert(prop != NULL);
	assert(prop->kind == SCORPI_PROP_U64);
	assert(prop->value.u64 == 4);

	assert(scorpi_create_pci_device("hostbridge", SCORPI_PCI_SLOT_AUTO,
	    &auto_pci) == SCORPI_OK);
	assert(auto_pci != NULL);
	assert(auto_pci->slot == SCORPI_PCI_SLOT_AUTO);

	assert(scorpi_create_usb_device("tablet", &usb) == SCORPI_OK);
	assert(usb != NULL);
	assert(scorpi_device_set_prop_u64(usb, "port", 1) == SCORPI_OK);
	prop = scorpi_device_find_prop(usb, "port");
	assert(prop != NULL);
	assert(prop->kind == SCORPI_PROP_U64);
	assert(prop->value.u64 == 1);

	assert(scorpi_create_lpc_device("vm-control", &lpc) == SCORPI_OK);
	assert(lpc != NULL);
	assert(scorpi_device_set_prop_string(lpc, "socket",
	    "/tmp/scorpi.sock") == SCORPI_OK);
	prop = scorpi_device_find_prop(lpc, "socket");
	assert(prop != NULL);
	assert(prop->kind == SCORPI_PROP_STRING);

	scorpi_destroy_device(lpc);
	scorpi_destroy_device(usb);
	scorpi_destroy_device(auto_pci);
	scorpi_destroy_device(pci);
	scorpi_destroy_device(NULL);
	return (0);
}
