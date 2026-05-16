/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/param.h>

#include <vmmapi.h>

#include "pci_emul.h"

static int
pci_lpc_init(struct pci_devinst *pi, nvlist_t *nvl __unused)
{
	pci_set_cfgdata16(pi, PCIR_VENDOR, 0x8086);
	pci_set_cfgdata16(pi, PCIR_DEVICE, 0x7000);
	pci_set_cfgdata8(pi, PCIR_HDRTYPE, PCIM_HDRTYPE_NORMAL);
	pci_set_cfgdata8(pi, PCIR_CLASS, PCIC_BRIDGE);
	pci_set_cfgdata8(pi, PCIR_SUBCLASS, PCIS_BRIDGE_ISA);
	pci_set_cfgdata8(pi, PCIR_PROGIF, 0);

	return (0);
}

static const struct pci_devemu pci_de_lpc = {
	.pe_emu = "lpc",
	.pe_init = pci_lpc_init,
};
PCI_EMUL_SET(pci_de_lpc);
