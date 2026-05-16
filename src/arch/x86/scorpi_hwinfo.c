/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/param.h>
#include <err.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <support/endian.h>

#include "vmm.h"

#include <vmmapi.h>

#include "bhyverun.h"
#include "bootrom.h"
#include "config.h"
#include "mem.h"
#include "pci_emul.h"
#include "qemu_fwcfg.h"
#include "tpm_crb.h"
#include "tpm_tis.h"
#include "scorpi_hwinfo.h"

#include <scorpi_x64_hwinfo.h>

#define SCORPI_HWINFO_LAPIC_BASE		0xFEE00000ULL
#define SCORPI_HWINFO_LAPIC_SIZE		0x1000
#define SCORPI_HWINFO_IOAPIC_BASE	0xFEC00000ULL
#define SCORPI_HWINFO_IOAPIC_SIZE	0x1000
#define SCORPI_HWINFO_IOAPIC_ID		0
#define SCORPI_HWINFO_IOAPIC_GSI_BASE	0
#define SCORPI_HWINFO_IOAPIC_GSI_COUNT	24
#define SCORPI_HWINFO_ECAM_SIZE		((PCI_BUSMAX + 1) * 1024 * 1024ULL)
#define SCORPI_HWINFO_LOWMEM_HOLE_BASE	0xA0000ULL
#define SCORPI_HWINFO_LOWMEM_HOLE_END	0x100000ULL

#define SCORPI_HWINFO_RESET_BASE		0xF0000000ULL
#define SCORPI_HWINFO_RESET_SIZE		0x1000
#define SCORPI_HWINFO_RESET_OFF		0
#define SCORPI_HWINFO_SHUTDOWN_OFF	4
#define SCORPI_HWINFO_RESET_VALUE	1
#define SCORPI_HWINFO_SHUTDOWN_VALUE	1

struct scorpi_hwinfo_builder {
	uint8_t *data;
	size_t len;
	size_t cap;
	uint32_t count;
};

static int
scorpi_hwinfo_reset_handler(struct vcpu *vcpu, int dir, uint64_t addr, int size,
    uint64_t *val, void *arg1, long arg2)
{
	struct vmctx *ctx;
	uint64_t off;

	(void)vcpu;
	(void)arg2;

	ctx = arg1;
	if (dir == MEM_F_READ) {
		*val = 0;
		return (0);
	}

	if (size != sizeof(uint32_t)) {
		warnx("%s: invalid write size %d", __func__, size);
		return (-1);
	}

	off = addr - SCORPI_HWINFO_RESET_BASE;
	if (off == SCORPI_HWINFO_RESET_OFF && (uint32_t)*val == SCORPI_HWINFO_RESET_VALUE)
		return (vm_suspend(ctx, VM_SUSPEND_RESET));
	if (off == SCORPI_HWINFO_SHUTDOWN_OFF &&
	    (uint32_t)*val == SCORPI_HWINFO_SHUTDOWN_VALUE)
		return (vm_suspend(ctx, VM_SUSPEND_POWEROFF));

	return (0);
}

static struct scorpi_x64_hwinfo_entry
scorpi_hwinfo_entry(uint16_t type, uint32_t size)
{
	struct scorpi_x64_hwinfo_entry entry;

	memset(&entry, 0, sizeof(entry));
	entry.type = htole16(type);
	entry.size = htole32(size);

	return (entry);
}

static int
scorpi_hwinfo_append(struct scorpi_hwinfo_builder *b, const void *entry, size_t size)
{
	if ((size & 7) != 0)
		return (EINVAL);
	if (b->len + size > b->cap)
		return (ENOMEM);

	memcpy(b->data + b->len, entry, size);
	b->len += size;
	b->count++;

	return (0);
}

static int
scorpi_hwinfo_add_range(struct scorpi_hwinfo_builder *b, uint16_t type, uint64_t base,
    uint64_t size, uint32_t range_type)
{
	struct scorpi_x64_hwinfo_range range;

	if (size == 0)
		return (0);

	memset(&range, 0, sizeof(range));
	range.entry = scorpi_hwinfo_entry(type, sizeof(range));
	range.base = htole64(base);
	range.size = htole64(size);
	range.range_type = htole32(range_type);

	return (scorpi_hwinfo_append(b, &range, sizeof(range)));
}

static int
scorpi_hwinfo_add_ram(struct scorpi_hwinfo_builder *b, uint64_t base, uint64_t end,
    bool have_tpm, uint64_t tpm_base, uint64_t tpm_size)
{
	uint64_t tpm_end;

	if (end <= base)
		return (0);

	if (!have_tpm || tpm_size == 0)
		return (scorpi_hwinfo_add_range(b, SCORPI_X64_ENTRY_RAM_RANGE, base,
		    end - base, SCORPI_X64_RANGE_USABLE));

	tpm_end = tpm_base + tpm_size;
	if (tpm_end < tpm_base)
		tpm_end = UINT64_MAX;
	if (tpm_base >= end || tpm_end <= base)
		return (scorpi_hwinfo_add_range(b, SCORPI_X64_ENTRY_RAM_RANGE, base,
		    end - base, SCORPI_X64_RANGE_USABLE));

	if (tpm_base > base) {
		int error;

		error = scorpi_hwinfo_add_range(b, SCORPI_X64_ENTRY_RAM_RANGE, base,
		    MIN(tpm_base, end) - base, SCORPI_X64_RANGE_USABLE);
		if (error != 0)
			return (error);
	}
	if (tpm_end < end)
		return (scorpi_hwinfo_add_range(b, SCORPI_X64_ENTRY_RAM_RANGE, tpm_end,
		    end - tpm_end, SCORPI_X64_RANGE_USABLE));

	return (0);
}

static int
scorpi_hwinfo_add_cpu(struct scorpi_hwinfo_builder *b, uint32_t vcpuid)
{
	struct scorpi_x64_hwinfo_cpu cpu;

	memset(&cpu, 0, sizeof(cpu));
	cpu.entry = scorpi_hwinfo_entry(SCORPI_X64_ENTRY_CPU, sizeof(cpu));
	cpu.acpi_processor_uid = htole32(vcpuid);
	cpu.apic_id = htole32(vcpuid);

	return (scorpi_hwinfo_append(b, &cpu, sizeof(cpu)));
}

static uint32_t
scorpi_hwinfo_gsi_count(struct vmctx *ctx)
{
	int pincount;

	if (vm_ioapic_pincount(ctx, &pincount) == 0 && pincount > 0)
		return ((uint32_t)pincount);

	return (SCORPI_HWINFO_IOAPIC_GSI_COUNT);
}

static int
scorpi_hwinfo_add_apic(struct scorpi_hwinfo_builder *b, struct vmctx *ctx)
{
	struct scorpi_x64_hwinfo_apic apic;

	memset(&apic, 0, sizeof(apic));
	apic.entry = scorpi_hwinfo_entry(SCORPI_X64_ENTRY_APIC, sizeof(apic));
	apic.local_apic_base = htole64(SCORPI_HWINFO_LAPIC_BASE);
	apic.io_apic_base = htole64(SCORPI_HWINFO_IOAPIC_BASE);
	apic.io_apic_id = htole32(SCORPI_HWINFO_IOAPIC_ID);
	apic.gsi_base = htole32(SCORPI_HWINFO_IOAPIC_GSI_BASE);
	apic.gsi_count = htole32(scorpi_hwinfo_gsi_count(ctx));

	return (scorpi_hwinfo_append(b, &apic, sizeof(apic)));
}

static int
scorpi_hwinfo_add_ecam(struct scorpi_hwinfo_builder *b)
{
	struct scorpi_x64_hwinfo_pcie_ecam ecam;

	memset(&ecam, 0, sizeof(ecam));
	ecam.entry = scorpi_hwinfo_entry(SCORPI_X64_ENTRY_PCIE_ECAM, sizeof(ecam));
	ecam.end_bus = PCI_BUSMAX;
	ecam.base = htole64(PCI_EMUL_ECFG_BASE);
	ecam.size = htole64(SCORPI_HWINFO_ECAM_SIZE);

	return (scorpi_hwinfo_append(b, &ecam, sizeof(ecam)));
}

static int
scorpi_hwinfo_add_pci_win(struct scorpi_hwinfo_builder *b, uint32_t type, uint64_t base,
    uint64_t size)
{
	struct scorpi_x64_hwinfo_pci_window win;

	if (size == 0)
		return (0);

	memset(&win, 0, sizeof(win));
	win.entry = scorpi_hwinfo_entry(SCORPI_X64_ENTRY_PCI_WINDOW, sizeof(win));
	win.window_type = htole32(type);
	win.cpu_base = htole64(base);
	win.pci_base = htole64(base);
	win.size = htole64(size);

	return (scorpi_hwinfo_append(b, &win, sizeof(win)));
}

static uint32_t
scorpi_hwinfo_profile(void)
{
	const char *value;
	char *end;
	unsigned long profile;

	value = get_config_value("scorpi.x64.profile");
	if (value == NULL)
		value = get_config_value("scorpi.profile");
	if (value == NULL)
		value = get_config_value("profile");
	if (value == NULL)
		return (SCORPI_X64_PROFILE_LINUX_VIRTIO);

	if (strcasecmp(value, "linux-virtio") == 0 ||
	    strcasecmp(value, "linux") == 0)
		return (SCORPI_X64_PROFILE_LINUX_VIRTIO);
	if (strcasecmp(value, "windows-inbox") == 0 ||
	    strcasecmp(value, "windows") == 0)
		return (SCORPI_X64_PROFILE_WINDOWS_INBOX);

	errno = 0;
	profile = strtoul(value, &end, 0);
	if (errno == 0 && *end == '\0' &&
	    (profile == SCORPI_X64_PROFILE_LINUX_VIRTIO ||
	    profile == SCORPI_X64_PROFILE_WINDOWS_INBOX))
		return ((uint32_t)profile);

	warnx("%s: unknown profile \"%s\", using linux-virtio", __func__,
	    value);
	return (SCORPI_X64_PROFILE_LINUX_VIRTIO);
}

static bool
scorpi_hwinfo_tpm_range(uint64_t *base, uint64_t *size, uint32_t *intf_type)
{
	const char *intf;
	nvlist_t *nvl;

	nvl = find_config_node("tpm");
	if (nvl == NULL)
		return (false);

	intf = get_config_value_node(nvl, "intf");
	if (intf == NULL || strcmp(intf, "crb") == 0) {
		if (base != NULL)
			*base = TPM_CRB_ADDRESS;
		if (size != NULL)
			*size = TPM_CRB_MMIO_SIZE;
		if (intf_type != NULL)
			*intf_type = SCORPI_X64_TPM_INTERFACE_CRB;
		return (true);
	}
	if (strcmp(intf, "tis") == 0) {
		if (base != NULL)
			*base = TPM_TIS_ADDRESS;
		if (size != NULL)
			*size = TPM_TIS_MMIO_SIZE;
		if (intf_type != NULL)
			*intf_type = SCORPI_X64_TPM_INTERFACE_TIS;
		return (true);
	}

	warnx("%s: unsupported TPM interface \"%s\"", __func__, intf);
	return (false);
}

static int
scorpi_hwinfo_add_tpm(struct scorpi_hwinfo_builder *b)
{
	struct scorpi_x64_hwinfo_tpm tpm;
	uint64_t base, size;
	uint32_t intf_type;

	if (!scorpi_hwinfo_tpm_range(&base, &size, &intf_type))
		return (0);

	memset(&tpm, 0, sizeof(tpm));
	tpm.entry = scorpi_hwinfo_entry(SCORPI_X64_ENTRY_TPM, sizeof(tpm));
	tpm.base = htole64(base);
	tpm.size = htole32(size);
	tpm.interface_type = htole32(intf_type);

	return (scorpi_hwinfo_append(b, &tpm, sizeof(tpm)));
}

static int
scorpi_hwinfo_add_reset(struct scorpi_hwinfo_builder *b)
{
	struct scorpi_x64_hwinfo_reset reset;

	memset(&reset, 0, sizeof(reset));
	reset.entry = scorpi_hwinfo_entry(SCORPI_X64_ENTRY_RESET, sizeof(reset));
	reset.base = htole64(SCORPI_HWINFO_RESET_BASE);
	reset.size = htole32(SCORPI_HWINFO_RESET_SIZE);
	reset.reset_offset = htole32(SCORPI_HWINFO_RESET_OFF);
	reset.shutdown_offset = htole32(SCORPI_HWINFO_SHUTDOWN_OFF);
	reset.reset_value = htole32(SCORPI_HWINFO_RESET_VALUE);
	reset.shutdown_value = htole32(SCORPI_HWINFO_SHUTDOWN_VALUE);

	return (scorpi_hwinfo_append(b, &reset, sizeof(reset)));
}

static int
scorpi_hwinfo_reset_register(struct vmctx *ctx)
{
	static bool registered;
	struct mem_range mr;
	int error;

	if (registered)
		return (0);

	memset(&mr, 0, sizeof(mr));
	mr.name = "scorpi_hwinfo_reset";
	mr.base = SCORPI_HWINFO_RESET_BASE;
	mr.size = SCORPI_HWINFO_RESET_SIZE;
	mr.flags = MEM_F_RW;
	mr.handler = scorpi_hwinfo_reset_handler;
	mr.arg1 = ctx;

	error = register_mem(&mr);
	if (error == 0)
		registered = true;

	return (error);
}

static uint32_t
scorpi_hwinfo_cksum(const uint8_t *data, size_t size)
{
	size_t cksum_off;
	uint32_t cksum;

	cksum_off = offsetof(struct scorpi_x64_hwinfo_header, checksum32);
	cksum = 0;
	for (size_t i = 0; i < size; i++) {
		if (i >= cksum_off && i < cksum_off + sizeof(uint32_t))
			continue;
		cksum += data[i];
	}

	return (cksum);
}

static int
scorpi_hwinfo_build(struct vmctx *ctx, void **data, uint32_t *size)
{
	struct scorpi_x64_hwinfo_header header;
	struct scorpi_hwinfo_builder b;
	uint64_t high_base, pci64_base, tpm_base, tpm_size;
	uint64_t low_end, low_hole_end;
	uint64_t vars_base, vars_size;
	size_t low_size, high_size;
	bool have_tpm;
	int error;

	if (data == NULL || size == NULL)
		return (EINVAL);

	b.cap = sizeof(header) + (guest_ncpus + 32) * 64;
	b.data = calloc(1, b.cap);
	if (b.data == NULL)
		return (ENOMEM);
	b.len = sizeof(header);
	b.count = 0;

	low_size = vm_get_lowmem_size(ctx);
	high_base = vm_get_highmem_base(ctx);
	high_size = vm_get_highmem_size(ctx);
	have_tpm = scorpi_hwinfo_tpm_range(&tpm_base, &tpm_size, NULL);
	low_end = low_size;

#define SCORPI_HWINFO_ADD(expr)				\
	do {					\
		error = (expr);			\
		if (error != 0)			\
			goto fail;		\
	} while (0)

	SCORPI_HWINFO_ADD(scorpi_hwinfo_add_ram(&b, 0,
	    MIN(low_end, SCORPI_HWINFO_LOWMEM_HOLE_BASE), have_tpm, tpm_base,
	    tpm_size));
	if (low_end > SCORPI_HWINFO_LOWMEM_HOLE_BASE) {
		low_hole_end = MIN(low_end, SCORPI_HWINFO_LOWMEM_HOLE_END);
		SCORPI_HWINFO_ADD(scorpi_hwinfo_add_range(&b,
		    SCORPI_X64_ENTRY_RESERVED_RANGE, SCORPI_HWINFO_LOWMEM_HOLE_BASE,
		    low_hole_end - SCORPI_HWINFO_LOWMEM_HOLE_BASE,
		    SCORPI_X64_RANGE_RESERVED));
	}
	SCORPI_HWINFO_ADD(scorpi_hwinfo_add_ram(&b, SCORPI_HWINFO_LOWMEM_HOLE_END,
	    low_end, have_tpm, tpm_base, tpm_size));
	SCORPI_HWINFO_ADD(scorpi_hwinfo_add_range(&b, SCORPI_X64_ENTRY_RAM_RANGE, high_base,
	    high_size, SCORPI_X64_RANGE_USABLE));

	if (low_size < PCI_EMUL_MEMBASE32) {
		SCORPI_HWINFO_ADD(scorpi_hwinfo_add_range(&b,
		    SCORPI_X64_ENTRY_RESERVED_RANGE, low_size,
		    PCI_EMUL_MEMBASE32 - low_size, SCORPI_X64_RANGE_RESERVED));
	}
	SCORPI_HWINFO_ADD(scorpi_hwinfo_add_range(&b, SCORPI_X64_ENTRY_RESERVED_RANGE,
	    PCI_EMUL_ECFG_BASE, SCORPI_HWINFO_ECAM_SIZE,
	    SCORPI_X64_RANGE_RESERVED));
	if (bootrom_boot()) {
		SCORPI_HWINFO_ADD(scorpi_hwinfo_add_range(&b,
		    SCORPI_X64_ENTRY_RESERVED_RANGE, bootrom_rombase(),
		    bootrom_romsize(), SCORPI_X64_RANGE_RESERVED));
	}
	if (bootrom_vars(&vars_base, &vars_size) == 0) {
		SCORPI_HWINFO_ADD(scorpi_hwinfo_add_range(&b,
		    SCORPI_X64_ENTRY_RESERVED_RANGE, vars_base, vars_size,
		    SCORPI_X64_RANGE_RESERVED));
	}
	if (have_tpm) {
		SCORPI_HWINFO_ADD(scorpi_hwinfo_add_range(&b,
		    SCORPI_X64_ENTRY_RESERVED_RANGE, tpm_base, tpm_size,
		    SCORPI_X64_RANGE_RESERVED));
	}

	for (int vcpuid = 0; vcpuid < guest_ncpus; vcpuid++)
		SCORPI_HWINFO_ADD(scorpi_hwinfo_add_cpu(&b, vcpuid));

	SCORPI_HWINFO_ADD(scorpi_hwinfo_add_apic(&b, ctx));
	SCORPI_HWINFO_ADD(scorpi_hwinfo_add_ecam(&b));
	SCORPI_HWINFO_ADD(scorpi_hwinfo_add_pci_win(&b, SCORPI_X64_PCI_WINDOW_MMIO32,
	    PCI_EMUL_MEMBASE32, PCI_EMUL_MEMLIMIT32 - PCI_EMUL_MEMBASE32));

	pci64_base = roundup2(high_base + high_size, PCI_EMUL_MEMSIZE64);
	SCORPI_HWINFO_ADD(scorpi_hwinfo_add_pci_win(&b, SCORPI_X64_PCI_WINDOW_MMIO64,
	    pci64_base, PCI_EMUL_MEMSIZE64));
	SCORPI_HWINFO_ADD(scorpi_hwinfo_add_tpm(&b));
	SCORPI_HWINFO_ADD(scorpi_hwinfo_add_reset(&b));

#undef SCORPI_HWINFO_ADD

	if (b.len > UINT32_MAX) {
		error = EOVERFLOW;
		goto fail;
	}

	memset(&header, 0, sizeof(header));
	memcpy(header.magic, SCORPI_X64_HWINFO_MAGIC, sizeof(header.magic));
	header.major = htole16(SCORPI_X64_HWINFO_MAJOR);
	header.minor = htole16(SCORPI_X64_HWINFO_MINOR);
	header.header_size = htole32(sizeof(header));
	header.total_size = htole32((uint32_t)b.len);
	header.profile = htole32(scorpi_hwinfo_profile());
	header.entry_count = htole32(b.count);
	header.entries_offset = htole64(sizeof(header));
	memcpy(b.data, &header, sizeof(header));

	((struct scorpi_x64_hwinfo_header *)b.data)->checksum32 =
	    htole32(scorpi_hwinfo_cksum(b.data, b.len));

	*data = b.data;
	*size = (uint32_t)b.len;
	return (0);

fail:
	free(b.data);
	return (error);
}

int
scorpi_hwinfo_add_fwcfg(struct vmctx *ctx)
{
	void *data;
	uint32_t size;
	int error;

	error = scorpi_hwinfo_reset_register(ctx);
	if (error != 0)
		return (error);

	error = scorpi_hwinfo_build(ctx, &data, &size);
	if (error != 0)
		return (error);

	error = qemu_fwcfg_add_file(SCORPI_X64_HWINFO_FWCFG_FILE, size, data);
	if (error != 0)
		free(data);

	return (error);
}
