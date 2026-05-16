/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <string.h>

#include "bootrom_arch.h"

void
bootrom_copyrom(char *dst, size_t dst_len, const void *src, size_t src_len)
{
	(void)dst_len;
	memcpy(dst, src, src_len);
}
