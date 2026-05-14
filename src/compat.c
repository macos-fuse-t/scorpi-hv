/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Alex Fishman <alex@fuse-t.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "compat.h"
#include "debug.h"

#ifdef __linux__
#include <sched.h>
#include <string.h>
extern char *program_invocation_short_name;
#elif __APPLE__
#include <sys/syslimits.h>
#include <mach/mach.h>
#endif

#ifdef __linux__
const char *
getprogname(void)
{
	if (program_invocation_short_name != NULL)
		return (program_invocation_short_name);
	return ("scorpi-hv");
}
#endif

// Cross-platform function to set the thread name
int
compat_set_thread_name(pthread_t thread, const char *name)
{
	int res;
#ifdef __linux__
	if ((res = pthread_setname_np(pthread_self(), name)) != 0) {
		EPRINTLN("pthread_setname_np failed");
	}
#elif __APPLE__
	if (pthread_equal(pthread_self(), thread)) {
		res = pthread_setname_np(name);
	} else {
		// EPRINTLN("Cannot set name for threads other than the current
		// thread on macOS");
		res = -1;
	}
#else
	EPRINTLN("Setting thread name is not supported on this platform");
	res = -1;
#endif
	return res;
}

// Cross-platform function to set thread affinity
int
compat_set_thread_affinity(pthread_t thread, int core_id, cpuset_t *cpuset)
{
#ifdef __linux__
	int res;
	cpu_set_t linux_cpuset;

	memset(&linux_cpuset, 0, sizeof(linux_cpuset));
	for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, cpuset))
			__CPU_SET_S(cpu, sizeof(linux_cpuset), &linux_cpuset);
	}
	if ((res = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t),
		 &linux_cpuset)) != 0) {
		EPRINTLN("pthread_setaffinity_np failed");
	}
	return res;
#elif __APPLE__
	// macOS: Use thread_policy_set (suggest affinity, no strict binding)
	if (pthread_equal(pthread_self(), thread)) {
		thread_port_t mach_thread = mach_thread_self();

		thread_affinity_policy_data_t policy;
		policy.affinity_tag = core_id;

		kern_return_t kr = thread_policy_set(mach_thread,
		    THREAD_AFFINITY_POLICY, (thread_policy_t)&policy,
		    THREAD_AFFINITY_POLICY_COUNT);
		if (kr != KERN_SUCCESS) {
			EPRINTLN("thread_policy_set failed on macOS");
			return -1;
		}
		return 0;
	} else {
		EPRINTLN(
		    "Cannot set affinity for threads other than the current thread on macOS");
		return -1;
	}
#else
	// Unsupported platform
	EPRINTLN("Setting thread affinity is not supported on this platform");
	return -1;
#endif
}
