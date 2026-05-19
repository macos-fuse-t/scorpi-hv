/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <string.h>

#include "bootrom.h"
#include "bootrom_arch.h"

size_t
bootrom_arch_alloc_size(size_t rom_size, size_t bootrom_size)
{
	(void)bootrom_size;
	return (rom_size);
}

int
bootrom_arch_alloc_flags(void)
{
	return (BOOTROM_ALLOC_TOP);
}

void
bootrom_copyrom(char *dst, size_t dst_len, const void *src, size_t src_len)
{
	memcpy(dst + dst_len - src_len, src, src_len);
}
