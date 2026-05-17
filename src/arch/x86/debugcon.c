/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/types.h>

#include <unistd.h>

#include "arch/x86/inout.h"

#define DEBUGCON_PORT	0x402
#define DEBUGCON_MAGIC	0xe9

static int
debugcon_handler(struct vmctx *ctx __unused, int in, int port __unused,
    int bytes, uint32_t *eax, void *arg __unused)
{
	uint32_t val;
	int i;
	unsigned char ch;

	if (in) {
		*eax = DEBUGCON_MAGIC;
		return (0);
	}

	val = *eax;
	for (i = 0; i < bytes; i++) {
		ch = val & 0xff;
		(void)write(STDERR_FILENO, &ch, 1);
		val >>= 8;
	}

	return (0);
}

INOUT_PORT(debugcon, DEBUGCON_PORT, IOPORT_F_INOUT, debugcon_handler);
