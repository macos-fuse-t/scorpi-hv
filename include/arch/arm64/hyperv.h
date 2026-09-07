/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef SCORPI_ARM64_HYPERV_H
#define SCORPI_ARM64_HYPERV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/* Minimal partition-wide state; no synthetic interrupt/timer support. */
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
};

typedef void *(*arm64_hyperv_map_fn)(void *, uint64_t, size_t);
int arm64_hyperv_init(struct arm64_hyperv *);
void arm64_hyperv_reset(struct arm64_hyperv *);
void arm64_hyperv_destroy(struct arm64_hyperv *);
/* x contains X0..X17; false means this is not a Hyper-V call. */
bool arm64_hyperv_call(struct arm64_hyperv *, unsigned int imm,
    uint64_t x[18], unsigned int cpu, unsigned int cpu_count,
    arm64_hyperv_map_fn, void *);

#endif
