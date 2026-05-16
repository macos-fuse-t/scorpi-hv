/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2011 NetApp, Inc.
 * All rights reserved.
 */

#include <sys/param.h>

#include "vmm.h"

#include <assert.h>
#include <string.h>

#include <vmmapi.h>

#include "arch/x86/inout.h"
#include "config.h"

SET_DECLARE(inout_port_set, struct inout_port);

#define MAX_IOPORTS	(1 << 16)

#define VERIFY_IOPORT(port, size) \
	assert((port) >= 0 && (size) > 0 && ((port) + (size)) <= MAX_IOPORTS)

static struct {
	const char *name;
	int flags;
	inout_func_t handler;
	void *arg;
} inout_handlers[MAX_IOPORTS];

static int
default_inout(struct vmctx *ctx __unused, int in, int port __unused,
    int bytes, uint32_t *eax, void *arg __unused)
{
	if (in) {
		switch (bytes) {
		case 4:
			*eax = 0xffffffff;
			break;
		case 2:
			*eax = 0xffff;
			break;
		case 1:
			*eax = 0xff;
			break;
		}
	}

	return (0);
}

static void
register_default_iohandler(int start, int size)
{
	struct inout_port iop;

	VERIFY_IOPORT(start, size);

	memset(&iop, 0, sizeof(iop));
	iop.name = "default";
	iop.port = start;
	iop.size = size;
	iop.flags = IOPORT_F_INOUT | IOPORT_F_DEFAULT;
	iop.handler = default_inout;

	register_inout(&iop);
}

int
emulate_inout(struct vmctx *ctx, struct vcpu *vcpu, struct vm_exit *vmexit)
{
	uint32_t eax, val;
	inout_func_t handler;
	void *arg;
	int bytes, flags, in, port, retval;

	(void)vcpu;

	bytes = vmexit->u.inout.bytes;
	in = vmexit->u.inout.in;
	port = vmexit->u.inout.port;

	assert(port < MAX_IOPORTS);
	assert(bytes == 1 || bytes == 2 || bytes == 4);

	handler = inout_handlers[port].handler;
	if (handler == default_inout &&
	    get_config_bool_default("x86.strictio", false)) {
		return (-1);
	}

	flags = inout_handlers[port].flags;
	arg = inout_handlers[port].arg;

	if (in) {
		if ((flags & IOPORT_F_IN) == 0)
			return (-1);
	} else if ((flags & IOPORT_F_OUT) == 0) {
		return (-1);
	}

	eax = vmexit->u.inout.eax;
	val = eax;
	retval = handler(ctx, in, port, bytes, &val, arg);
	if (retval == 0 && in) {
		eax &= ~((1ULL << (bytes * 8)) - 1);
		eax |= val & ((1ULL << (bytes * 8)) - 1);
		vmexit->u.inout.eax = eax;
	}

	return (retval);
}

void
init_inout(void)
{
	struct inout_port **iopp, *iop;

	register_default_iohandler(0, MAX_IOPORTS);

	SET_FOREACH(iopp, inout_port_set) {
		iop = *iopp;
		assert(iop->port < MAX_IOPORTS);
		inout_handlers[iop->port].name = iop->name;
		inout_handlers[iop->port].flags = iop->flags;
		inout_handlers[iop->port].handler = iop->handler;
		inout_handlers[iop->port].arg = NULL;
	}
}

int
register_inout(struct inout_port *iop)
{
	int i;

	VERIFY_IOPORT(iop->port, iop->size);

	if ((iop->flags & IOPORT_F_DEFAULT) == 0) {
		for (i = iop->port; i < iop->port + iop->size; i++) {
			if ((inout_handlers[i].flags & IOPORT_F_DEFAULT) == 0)
				return (-1);
		}
	}

	for (i = iop->port; i < iop->port + iop->size; i++) {
		inout_handlers[i].name = iop->name;
		inout_handlers[i].flags = iop->flags;
		inout_handlers[i].handler = iop->handler;
		inout_handlers[i].arg = iop->arg;
	}

	return (0);
}

int
unregister_inout(struct inout_port *iop)
{
	VERIFY_IOPORT(iop->port, iop->size);
	assert(inout_handlers[iop->port].name == iop->name);

	register_default_iohandler(iop->port, iop->size);

	return (0);
}
