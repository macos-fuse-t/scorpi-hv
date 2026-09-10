/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef SCORPI_ARM64_MEMORY_LAYOUT_H
#define SCORPI_ARM64_MEMORY_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

/* Below PCI MMIO and above the platform peripherals and GIC. */
#define ARM64_LOWMEM_BASE UINT64_C(0x40000000)
#define ARM64_LOWMEM_SIZE UINT64_C(0x20000000)
#define ARM64_HIGHMEM_BASE UINT64_C(0x100000000)
/* EDK2 keeps its boot stack and permanent PEI memory in the high bank. */
#define ARM64_FIRMWARE_MEMORY_SIZE UINT64_C(0x04000000)

static inline size_t
arm64_lowmem_size(size_t total)
{

	if (total <= ARM64_FIRMWARE_MEMORY_SIZE)
		return (0);
	total -= ARM64_FIRMWARE_MEMORY_SIZE;
	return (total < ARM64_LOWMEM_SIZE ? total : ARM64_LOWMEM_SIZE);
}

#endif
