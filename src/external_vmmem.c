/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <stdint.h>
#include <stdio.h>
#include <uuid/uuid.h>

#include "external_vmmem.h"
#include "vmmapi.h"

extern uuid_t vm_uuid;

void
external_vmmem_shm_name(char *buf, size_t len, const char *suffix)
{
	uint64_t hash;

	hash = 1469598103934665603ULL;
	for (size_t i = 0; i < sizeof(vm_uuid); i++) {
		hash ^= vm_uuid[i];
		hash *= 1099511628211ULL;
	}
	snprintf(buf, len, "/svm-%016llx-%s", (unsigned long long)hash, suffix);
}

static void
external_vmmem_add_region(
    struct scorpi_virtio_external_transport_desc *transport,
    uint64_t guest_phys_addr, uint64_t size, const char *suffix)
{
	struct scorpi_virtio_external_memory_region_desc *region;

	if (size == 0 ||
	    transport->memory_region_count >=
		SCORPI_VIRTIO_EXTERNAL_MAX_MEMORY_REGIONS)
		return;

	region = &transport->memory_regions[transport->memory_region_count++];
	region->index = transport->memory_region_count - 1;
	region->guest_phys_addr = guest_phys_addr;
	region->size = size;
	region->shm_offset = 0;
	external_vmmem_shm_name(region->shm_name, sizeof(region->shm_name),
	    suffix);
}

void
external_vmmem_fill_transport(struct vmctx *ctx,
    struct scorpi_virtio_external_transport_desc *transport)
{
	external_vmmem_add_region(transport, 0, vm_get_lowmem_size(ctx), "low");
	external_vmmem_add_region(transport, vm_get_highmem_base(ctx),
	    vm_get_highmem_size(ctx), "high");
}
