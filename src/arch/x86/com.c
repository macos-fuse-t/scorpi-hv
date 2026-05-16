/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2013 Neel Natu <neel@freebsd.org>
 * Copyright (c) 2013 Tycho Nightingale <tycho.nightingale@pluribusnetworks.com>
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/param.h>

#include <assert.h>
#include <strings.h>

#include <vmmapi.h>

#include "arch/x86/com.h"
#include "arch/x86/inout.h"
#include "config.h"
#include "debug.h"
#include "uart_emul.h"

struct com_softc {
	struct uart_ns16550_softc *uart;
	int	iobase;
	int	irq;
};

static struct com_softc com_sc;

static void
com_intr_assert(void *arg __unused)
{
}

static void
com_intr_deassert(void *arg __unused)
{
}

static int
com_handler(struct vmctx *ctx __unused, int in, int port, int bytes,
    uint32_t *eax, void *arg)
{
	struct com_softc *sc;
	int off;

	sc = arg;
	off = port - sc->iobase;

	switch (bytes) {
	case 1:
		if (in)
			*eax = uart_ns16550_read(sc->uart, off);
		else
			uart_ns16550_write(sc->uart, off, *eax);
		break;
	case 2:
		if (in) {
			*eax = uart_ns16550_read(sc->uart, off);
			*eax |= uart_ns16550_read(sc->uart, off + 1) << 8;
		} else {
			uart_ns16550_write(sc->uart, off, *eax);
			uart_ns16550_write(sc->uart, off + 1, *eax >> 8);
		}
		break;
	default:
		return (-1);
	}

	return (0);
}

int
com_init(struct vmctx *ctx __unused)
{
	struct inout_port iop;
	struct com_softc *sc;
	const char *path;
	int error;

	path = get_config_value("console");
	if (path == NULL || strcasecmp(path, "off") == 0)
		return (0);

	sc = &com_sc;
	if (uart_legacy_alloc(0, &sc->iobase, &sc->irq) != 0) {
		EPRINTLN("Unable to allocate com1 resources");
		return (-1);
	}

	sc->uart = uart_ns16550_init(com_intr_assert, com_intr_deassert, sc);
	if (sc->uart == NULL) {
		EPRINTLN("Unable to initialize com1");
		return (-1);
	}

	if (uart_ns16550_tty_open(sc->uart, path) != 0) {
		EPRINTLN("Unable to initialize backend '%s' for com1", path);
		return (-1);
	}

	bzero(&iop, sizeof(iop));
	iop.name = "com1";
	iop.port = sc->iobase;
	iop.size = UART_NS16550_IO_BAR_SIZE;
	iop.flags = IOPORT_F_INOUT;
	iop.handler = com_handler;
	iop.arg = sc;

	error = register_inout(&iop);
	assert(error == 0);

	return (0);
}
