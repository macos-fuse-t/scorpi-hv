/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <vmmapi.h>

#include "qemu_fwcfg.h"
#include "scorpi_hwinfo.h"

int
qemu_fwcfg_arch_init(struct vmctx *ctx)
{
	return (scorpi_hwinfo_add_fwcfg(ctx));
}
