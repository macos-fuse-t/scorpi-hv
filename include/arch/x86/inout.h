/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2011 NetApp, Inc.
 * All rights reserved.
 */

#ifndef _ARCH_X86_INOUT_H_
#define _ARCH_X86_INOUT_H_

#include <sys/param.h>

#include <stdint.h>

#include <support/linker_set.h>

struct vcpu;
struct vmctx;
struct vm_exit;

typedef int (*inout_func_t)(struct vmctx *ctx, int in, int port, int bytes,
    uint32_t *eax, void *arg);

struct inout_port {
	const char *name;
	int port;
	int size;
	int flags;
	inout_func_t handler;
	void *arg;
};

#define IOPORT_F_IN	0x1
#define IOPORT_F_OUT	0x2
#define IOPORT_F_INOUT	(IOPORT_F_IN | IOPORT_F_OUT)

#define IOPORT_F_DEFAULT	0x80000000

#define INOUT_PORT(name, port, flags, handler)				\
	static struct inout_port __CONCAT(__inout_port, __LINE__) = {	\
		#name, (port), 1, (flags), (handler), 0		\
	};								\
	DATA_SET(inout_port_set, __CONCAT(__inout_port, __LINE__))

void init_inout(void);
int emulate_inout(struct vmctx *ctx, struct vcpu *vcpu, struct vm_exit *vmexit);
int register_inout(struct inout_port *iop);
int unregister_inout(struct inout_port *iop);

#endif /* _ARCH_X86_INOUT_H_ */
