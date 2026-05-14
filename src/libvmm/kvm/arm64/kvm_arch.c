/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include "../internal.h"

const char *
kvm_arch_backend_name(void)
{
	return ("kvm/arm64");
}
