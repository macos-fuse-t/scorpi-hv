/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#ifndef _SCORPI_X64_HWINFO_H_
#define _SCORPI_X64_HWINFO_H_

#include <stdint.h>

#define SCORPI_X64_HWINFO_FWCFG_FILE	"opt/scorpi/x64-hardware-info"
#define SCORPI_X64_HWINFO_MAGIC		"SXHI"
#define SCORPI_X64_HWINFO_MAJOR		1
#define SCORPI_X64_HWINFO_MINOR		0

#define SCORPI_X64_HWINFO_HEADER_SIZE	40
#define SCORPI_X64_HWINFO_ENTRY_SIZE	8

#if defined(__GNUC__) || defined(__clang__)
#define SCORPI_X64_PACKED	__attribute__((__packed__))
#else
#define SCORPI_X64_PACKED
#endif

enum scorpi_x64_profile {
	SCORPI_X64_PROFILE_LINUX_VIRTIO = 1,
	SCORPI_X64_PROFILE_WINDOWS_INBOX = 2,
};

enum scorpi_x64_hwinfo_entry_type {
	SCORPI_X64_ENTRY_RAM_RANGE = 1,
	SCORPI_X64_ENTRY_RESERVED_RANGE = 2,
	SCORPI_X64_ENTRY_CPU = 3,
	SCORPI_X64_ENTRY_APIC = 4,
	SCORPI_X64_ENTRY_PCIE_ECAM = 5,
	SCORPI_X64_ENTRY_PCI_WINDOW = 6,
	SCORPI_X64_ENTRY_DEVICE = 7,
	SCORPI_X64_ENTRY_FRAMEBUFFER = 8,
	SCORPI_X64_ENTRY_TPM = 9,
	SCORPI_X64_ENTRY_RESET = 10,
};

enum scorpi_x64_range_type {
	SCORPI_X64_RANGE_USABLE = 1,
	SCORPI_X64_RANGE_RESERVED = 2,
	SCORPI_X64_RANGE_ACPI_RECLAIM = 3,
	SCORPI_X64_RANGE_ACPI_NVS = 4,
};

enum scorpi_x64_pci_window_type {
	SCORPI_X64_PCI_WINDOW_MMIO32 = 1,
	SCORPI_X64_PCI_WINDOW_MMIO64 = 2,
};

enum scorpi_x64_device_type {
	SCORPI_X64_DEVICE_VIRTIO_BLK = 1,
	SCORPI_X64_DEVICE_VIRTIO_NET = 2,
	SCORPI_X64_DEVICE_VIRTIO_GPU = 3,
	SCORPI_X64_DEVICE_AHCI = 4,
	SCORPI_X64_DEVICE_XHCI = 5,
	SCORPI_X64_DEVICE_USB_HID = 6,
	SCORPI_X64_DEVICE_USB_NET = 7,
};

enum scorpi_x64_tpm_interface {
	SCORPI_X64_TPM_INTERFACE_CRB = 1,
	SCORPI_X64_TPM_INTERFACE_TIS = 2,
};

struct SCORPI_X64_PACKED scorpi_x64_hwinfo_header {
	char	 magic[4];
	uint16_t major;
	uint16_t minor;
	uint32_t header_size;
	uint32_t total_size;
	uint32_t checksum32;
	uint32_t flags;
	uint32_t profile;
	uint32_t entry_count;
	uint64_t entries_offset;
};

struct SCORPI_X64_PACKED scorpi_x64_hwinfo_entry {
	uint16_t type;
	uint16_t flags;
	uint32_t size;
};

struct SCORPI_X64_PACKED scorpi_x64_hwinfo_range {
	struct scorpi_x64_hwinfo_entry entry;
	uint64_t base;
	uint64_t size;
	uint32_t range_type;
	uint32_t flags;
};

struct SCORPI_X64_PACKED scorpi_x64_hwinfo_cpu {
	struct scorpi_x64_hwinfo_entry entry;
	uint32_t acpi_processor_uid;
	uint32_t apic_id;
	uint32_t flags;
	uint32_t reserved;
};

struct SCORPI_X64_PACKED scorpi_x64_hwinfo_apic {
	struct scorpi_x64_hwinfo_entry entry;
	uint64_t local_apic_base;
	uint64_t io_apic_base;
	uint32_t io_apic_id;
	uint32_t gsi_base;
	uint32_t gsi_count;
	uint32_t flags;
};

struct SCORPI_X64_PACKED scorpi_x64_hwinfo_pcie_ecam {
	struct scorpi_x64_hwinfo_entry entry;
	uint16_t segment;
	uint8_t start_bus;
	uint8_t end_bus;
	uint32_t reserved;
	uint64_t base;
	uint64_t size;
};

struct SCORPI_X64_PACKED scorpi_x64_hwinfo_pci_window {
	struct scorpi_x64_hwinfo_entry entry;
	uint32_t window_type;
	uint32_t flags;
	uint64_t cpu_base;
	uint64_t pci_base;
	uint64_t size;
};

struct SCORPI_X64_PACKED scorpi_x64_hwinfo_device {
	struct scorpi_x64_hwinfo_entry entry;
	uint16_t device_type;
	uint16_t bus;
	uint16_t slot;
	uint16_t function;
	uint32_t flags;
	uint32_t boot_index;
};

struct SCORPI_X64_PACKED scorpi_x64_hwinfo_framebuffer {
	struct scorpi_x64_hwinfo_entry entry;
	uint64_t base;
	uint64_t size;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
};

struct SCORPI_X64_PACKED scorpi_x64_hwinfo_tpm {
	struct scorpi_x64_hwinfo_entry entry;
	uint64_t base;
	uint32_t size;
	uint32_t interface_type;
	uint32_t flags;
	uint32_t reserved;
};

struct SCORPI_X64_PACKED scorpi_x64_hwinfo_reset {
	struct scorpi_x64_hwinfo_entry entry;
	uint64_t base;
	uint32_t size;
	uint32_t reset_offset;
	uint32_t shutdown_offset;
	uint32_t reset_value;
	uint32_t shutdown_value;
	uint32_t flags;
};

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(struct scorpi_x64_hwinfo_header) ==
    SCORPI_X64_HWINFO_HEADER_SIZE, "unexpected Scorpi X64 hwinfo header size");
_Static_assert(sizeof(struct scorpi_x64_hwinfo_entry) ==
    SCORPI_X64_HWINFO_ENTRY_SIZE, "unexpected Scorpi X64 hwinfo entry size");
#endif

#undef SCORPI_X64_PACKED

#endif /* _SCORPI_X64_HWINFO_H_ */
