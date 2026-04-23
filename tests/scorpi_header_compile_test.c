/* Compile-only coverage for the public scorpi header. */

#include "scorpi.h"

int
main(void)
{
	scorpi_vm_t *vm = 0;
	scorpi_device_t *dev = 0;
	scorpi_error_t err = SCORPI_OK;
	uint64_t slot = SCORPI_PCI_SLOT_AUTO;

	(void)vm;
	(void)dev;
	(void)err;
	(void)slot;
	return (0);
}
