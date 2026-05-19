/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#pragma once

#include <stddef.h>

size_t	bootrom_arch_alloc_size(size_t rom_size, size_t bootrom_size);
int	bootrom_arch_alloc_flags(void);
void	bootrom_copyrom(char *dst, size_t dst_len, const void *src,
	    size_t src_len);
