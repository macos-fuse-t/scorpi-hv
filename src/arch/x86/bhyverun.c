/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2011 NetApp, Inc.
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/param.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include <vmmapi.h>

#include "arch/x86/inout.h"
#include "bhyverun.h"
#include "bootrom.h"
#include "config.h"
#include "debug.h"
#include "pci_irq.h"
#include "qemu_fwcfg.h"
#include "smbiostbl.h"
#include <support/pci_reg.h>

#ifdef __linux__
const char *getprogname(void);
#endif

void
bhyve_init_config(void)
{
	init_config();

	set_config_bool("acpi_tables", false);
	set_config_bool("acpi_tables_in_memory", false);
	set_config_value("memory.size", "256M");
	set_config_bool("virtio_msix", true);
	set_config_bool("x86.strictio", false);
	set_config_bool("x86.vmexit_on_hlt", false);
	set_config_bool("x86.vmexit_on_pause", false);
	set_config_bool("x86.hypervisor", true);
	set_config_bool("x86.x2apic", false);
}

void
bhyve_usage(int code)
{
	const char *progname;

	progname = getprogname();
	fprintf(stderr,
	    "Usage: %s -f yaml_file\n"
	    "       %s -h\n"
	    "       -f: load VM configuration from YAML\n"
	    "       -h: help\n",
	    progname, progname);
	exit(code);
}

void
bhyve_optparse(int argc, char **argv)
{
	int c;

	while ((c = getopt(argc, argv, "f:h")) != -1) {
		switch (c) {
		case 'f':
			if (bhyve_get_yaml_config_file() != NULL)
				errx(EX_USAGE, "duplicate -f option");
			bhyve_set_yaml_config_file(optarg);
			break;
		case 'h':
			bhyve_usage(0);
		default:
			bhyve_usage(1);
		}
	}
}

void
bhyve_init_vcpu(struct vcpu *vcpu)
{
	enum x2apic_state x2apic_state;
	int error, tmp;

	if (vm_vcpu_init(vcpu) != 0)
		err(EX_OSERR, "failed to init vcpu");

	if (get_config_bool_default("x86.vmexit_on_hlt", false)) {
		error = vm_get_capability(vcpu, VM_CAP_HALT_EXIT, &tmp);
		if (error < 0)
			errx(EX_OSERR, "VM exit on HLT not supported");
		vm_set_capability(vcpu, VM_CAP_HALT_EXIT, 1);
	}

	if (get_config_bool_default("x86.vmexit_on_pause", false)) {
		error = vm_get_capability(vcpu, VM_CAP_PAUSE_EXIT, &tmp);
		if (error < 0)
			errx(EX_OSERR, "VM exit on PAUSE not supported");
		vm_set_capability(vcpu, VM_CAP_PAUSE_EXIT, 1);
	}

	x2apic_state = get_config_bool_default("x86.x2apic", false) ?
	    X2APIC_ENABLED : X2APIC_DISABLED;
	error = vm_set_x2apic_state(vcpu, x2apic_state);
	if (error != 0)
		errx(EX_OSERR, "unable to set x2APIC state (%d)", error);

	(void)vm_set_capability(vcpu, VM_CAP_ENABLE_INVPCID, 1);
	(void)vm_set_capability(vcpu, VM_CAP_IPI_EXIT, 1);
}

void
bhyve_start_vcpu(struct vcpu *vcpu, bool bsp)
{
	int error;

	if (bsp) {
		if (bootrom_boot()) {
			error = vm_set_capability(vcpu,
			    VM_CAP_UNRESTRICTED_GUEST, 1);
			if (error != 0)
				err(EX_OSERR,
				    "ROM boot requires unrestricted guest");
			error = vcpu_reset(vcpu);
			if (error != 0) {
				errno = error;
				err(EX_OSERR, "failed to reset BSP vcpu");
			}
		}
	} else {
		bhyve_init_vcpu(vcpu);
		error = vm_set_capability(vcpu, VM_CAP_UNRESTRICTED_GUEST, 1);
		assert(error == 0);
	}

	fbsdrun_addcpu(vcpu_id(vcpu));
}

static bool
x86_pci_uart_configured(void)
{
	const nvlist_t *bus, *func, *pci, *slot;
	const char *bus_name, *dev, *func_name, *slot_name;
	void *bus_cookie, *func_cookie, *slot_cookie;
	int bus_type, func_type, slot_type;

	pci = find_config_node("pci");
	if (pci == NULL)
		return (false);

	bus_cookie = NULL;
	while ((bus_name = nvlist_next(pci, &bus_type, &bus_cookie)) != NULL) {
		if (bus_type != NV_TYPE_NVLIST)
			continue;
		bus = nvlist_get_nvlist(pci, bus_name);
		slot_cookie = NULL;
		while ((slot_name = nvlist_next(bus, &slot_type,
		    &slot_cookie)) != NULL) {
			if (slot_type != NV_TYPE_NVLIST)
				continue;
			slot = nvlist_get_nvlist(bus, slot_name);
			func_cookie = NULL;
			while ((func_name = nvlist_next(slot, &func_type,
			    &func_cookie)) != NULL) {
				if (func_type != NV_TYPE_NVLIST)
					continue;
				func = nvlist_get_nvlist(slot, func_name);
				dev = get_config_value_node(func, "device");
				if (dev != NULL && strcmp(dev, "pci-uart") == 0)
					return (true);
			}
		}
	}

	return (false);
}

static bool
x86_pci_slot_used(unsigned int slot)
{
	char path[32];

	snprintf(path, sizeof(path), "pci.0.%u", slot);
	return (find_config_node(path) != NULL);
}

static int
x86_console_slot(unsigned int *slotp)
{
	unsigned int slot;

	for (slot = 1; slot <= PCI_SLOTMAX; slot++) {
		if (!x86_pci_slot_used(slot)) {
			*slotp = slot;
			return (0);
		}
	}

	return (EX_USAGE);
}

int
bhyve_config_defaults(void)
{
	char path[64];
	const char *console;
	unsigned int slot;
	int error;

	console = get_config_value("console");
	if (console == NULL || strcmp(console, "off") == 0)
		return (0);
	if (strcmp(console, "stdio") != 0) {
		warnx("unsupported x86 console '%s'", console);
		return (EX_USAGE);
	}

	if (x86_pci_uart_configured())
		return (0);

	error = x86_console_slot(&slot);
	if (error != 0) {
		warnx("no free PCI slot for x86 console");
		return (error);
	}

	snprintf(path, sizeof(path), "pci.0.%u.0.device", slot);
	set_config_value(path, "pci-uart");
	snprintf(path, sizeof(path), "pci.0.%u.0.backend", slot);
	set_config_value(path, "stdio");
	return (0);
}

int
bhyve_init_platform(struct vmctx *ctx, struct vcpu *bsp __unused)
{
	void *smbios;
	size_t smbios_len;
	int error;

	init_inout();
	init_bootrom(ctx);

	error = bootrom_loadrom(ctx);
	if (error != 0) {
		warn("failed to load bootrom");
		return (error);
	}

	error = qemu_fwcfg_init(ctx, 0, 0);
	if (error != 0) {
		EPRINTLN("qemu fwcfg initialization error");
		return (error);
	}

	error = smbios_build(ctx, &smbios, &smbios_len);
	if (error != 0) {
		EPRINTLN("Could not build smbios");
		return (error);
	}
	error = qemu_fwcfg_add_file("etc/smbios/smbios-tables",
	    smbios_len, smbios);
	if (error != 0)
		return (error);

	return (qemu_fwcfg_add_file(QEMU_FWCFG_FILE_TABLE_LOADER, 0, NULL));
}

int
bhyve_init_platform_late(struct vmctx *ctx __unused, struct vcpu *bsp __unused)
{
	return (0);
}

bool
bhyve_msi_supported(void)
{
	return (get_config_bool_default("x86.msi", true));
}
