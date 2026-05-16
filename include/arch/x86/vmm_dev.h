/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#ifndef	_X86_VMM_DEV_H_
#define	_X86_VMM_DEV_H_

#include <support/cpuset.h>
#include <stddef.h>

struct vm_exit;

struct vm_run {
	int cpuid;
	cpuset_t *cpuset;
	size_t cpusetsize;
	struct vm_exit *vm_exit;
};

#define	VM_ACTIVE_CPUS		0
#define	VM_SUSPENDED_CPUS	1
#define	VM_DEBUG_CPUS		2

#endif
