/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/param.h>

#include <errno.h>
#include <stdlib.h>

#include "config.h"
#include "debug.h"
#include "pci_emul.h"
#include "uart_emul.h"

#define	PCI_UART_VENDOR		0x1D0F
#define	PCI_UART_DEVICE		0x8250
#define	PCI_UART_BAR		0
#define	PCI_UART_BAR_SIZE	16

struct pci_uart_softc {
	struct pci_devinst *pi;
	struct uart_ns16550_softc *uart;
};

static void
pci_uart_intr_assert(void *arg)
{
	struct pci_uart_softc *sc;

	sc = arg;
	if (sc->pi->pi_lintr.pin != 0)
		pci_lintr_assert(sc->pi);
}

static void
pci_uart_intr_deassert(void *arg)
{
	struct pci_uart_softc *sc;

	sc = arg;
	if (sc->pi->pi_lintr.pin != 0)
		pci_lintr_deassert(sc->pi);
}

static uint64_t
pci_uart_read(struct pci_devinst *pi, int baridx __unused, uint64_t offset,
    int size)
{
	struct pci_uart_softc *sc;
	uint64_t value;
	int i;

	sc = pi->pi_arg;
	value = 0;
	for (i = 0; i < size; i++) {
		if (offset + i < UART_NS16550_IO_BAR_SIZE)
			value |= (uint64_t)uart_ns16550_read(sc->uart,
			    offset + i) << (i * 8);
		else
			value |= 0xffULL << (i * 8);
	}

	return (value);
}

static void
pci_uart_write(struct pci_devinst *pi, int baridx __unused, uint64_t offset,
    int size, uint64_t value)
{
	struct pci_uart_softc *sc;
	int i;

	sc = pi->pi_arg;
	for (i = 0; i < size; i++) {
		if (offset + i < UART_NS16550_IO_BAR_SIZE)
			uart_ns16550_write(sc->uart, offset + i,
			    value >> (i * 8));
	}
}

static int
pci_uart_init(struct pci_devinst *pi, nvlist_t *nvl)
{
	struct pci_uart_softc *sc;
	const char *path;
	int error;

	sc = calloc(1, sizeof(*sc));
	if (sc == NULL)
		return (ENOMEM);

	sc->pi = pi;
	pi->pi_arg = sc;

	sc->uart = uart_ns16550_init(pci_uart_intr_assert,
	    pci_uart_intr_deassert, sc);
	if (sc->uart == NULL) {
		free(sc);
		return (ENOMEM);
	}

	path = get_config_value_node(nvl, "backend");
	if (path == NULL)
		path = get_config_value_node(nvl, "path");
	if (path == NULL)
		path = "stdio";

	if (uart_ns16550_tty_open(sc->uart, path) != 0) {
		EPRINTLN("Unable to initialize backend '%s' for pci-uart",
		    path);
		free(sc);
		return (EINVAL);
	}

	pci_set_cfgdata16(pi, PCIR_VENDOR, PCI_UART_VENDOR);
	pci_set_cfgdata16(pi, PCIR_DEVICE, PCI_UART_DEVICE);
	pci_set_cfgdata8(pi, PCIR_HDRTYPE, PCIM_HDRTYPE_NORMAL);
	pci_set_cfgdata8(pi, PCIR_CLASS, PCIC_SIMPLECOMM);
	pci_set_cfgdata8(pi, PCIR_SUBCLASS, PCIS_SIMPLECOMM_UART);
	pci_set_cfgdata8(pi, PCIR_PROGIF, PCIP_SIMPLECOMM_UART_16550A);
	pci_set_cfgdata8(pi, PCIR_REVID, 0);

	error = pci_emul_alloc_bar(pi, PCI_UART_BAR, PCIBAR_MEM32,
	    PCI_UART_BAR_SIZE);
	if (error != 0) {
		free(sc);
		return (error);
	}
	pci_lintr_request(pi);

	return (0);
}

static const struct pci_devemu pci_de_uart = {
	.pe_emu = "pci-uart",
	.pe_init = pci_uart_init,
	.pe_barread = pci_uart_read,
	.pe_barwrite = pci_uart_write,
};
PCI_EMUL_SET(pci_de_uart);
