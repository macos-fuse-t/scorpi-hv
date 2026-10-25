/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef SCORPI_ARM64_HYPERV_H
#define SCORPI_ARM64_HYPERV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>

typedef void (*arm64_hyperv_kick_fn)(void *);
typedef int (*arm64_hyperv_inject_fn)(void *, uint32_t);

struct arm64_hyperv_timer {
	uint64_t config, count, deadline;
	bool pending;
};
struct arm64_hyperv_vp {
	uint64_t scontrol, simp, siefp, sint[16];
	struct arm64_hyperv_timer timers[4];
	_Atomic bool timer_pending;
	arm64_hyperv_kick_fn kick;
	void *kick_arg;
};

/* Partition state and timers share a lock; GIC injection stays on the vCPU. */
struct arm64_hyperv {
	pthread_mutex_t lock;
	uint64_t guest_os_id;
	uint64_t partition_info_page;
	unsigned int register_errors;
	uint32_t failed_registers[16];
	bool vp_enlightened[64];
	uint64_t synthetic_vectors[64];
	uint64_t crash_params[5];
	bool discovered;
	bool timers_enabled, stopping, worker_started;
	uint64_t reference_epoch;
	pthread_cond_t changed;
	pthread_t worker;
	struct arm64_hyperv_vp vp[64];
	void *(*map)(void *, uint64_t, size_t);
	void *map_arg;
};

typedef void *(*arm64_hyperv_map_fn)(void *, uint64_t, size_t);
int arm64_hyperv_init(struct arm64_hyperv *);
void arm64_hyperv_reset(struct arm64_hyperv *);
void arm64_hyperv_destroy(struct arm64_hyperv *);
int arm64_hyperv_enable_timers(struct arm64_hyperv *, arm64_hyperv_map_fn, void *);
void arm64_hyperv_set_vp(struct arm64_hyperv *, unsigned int,
    arm64_hyperv_kick_fn, void *);
/* Called only by the owning vCPU before entering the guest. A failed injection
 * leaves the expiration pending and returns the backend error.
 */
int arm64_hyperv_process_timers(struct arm64_hyperv *, unsigned int,
    arm64_hyperv_inject_fn, void *);
/* x contains X0..X17; false means this is not a Hyper-V call. */
bool arm64_hyperv_call(struct arm64_hyperv *, unsigned int imm,
    uint64_t x[18], unsigned int cpu, unsigned int cpu_count,
    arm64_hyperv_map_fn, void *);

#endif
