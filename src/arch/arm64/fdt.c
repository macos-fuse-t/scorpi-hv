/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 The FreeBSD Foundation
 *
 * This software was developed by Andrew Turner under sponsorship from
 * the FreeBSD Foundation.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <libfdt.h>
#include <vmmapi.h>
#include "pci_emul.h"

#include "bhyverun.h"
#include "common.h"
#include "config.h"
#include "fdt.h"

#define SET_PROP_U32(prop, idx, val) \
	((uint32_t *)(prop))[(idx)] = cpu_to_fdt32(val)
#define SET_PROP_U64(prop, idx, val) \
	((uint64_t *)(prop))[(idx)] = cpu_to_fdt64(val)

#define GIC_SPI		    0
#define GIC_PPI		    1
#define IRQ_TYPE_LEVEL_HIGH 4
#define IRQ_TYPE_LEVEL_LOW  8

#define GIC_FIRST_PPI	    16
#define GIC_FIRST_SPI	    32

static void *fdtroot;
static uint32_t gic_phandle = 0;
static uint32_t apb_pclk_phandle;

static uint32_t
assign_phandle(void *fdt)
{
	static uint32_t next_phandle = 1;
	uint32_t phandle;

	phandle = next_phandle;
	next_phandle++;
	fdt_property_u32(fdt, "phandle", phandle);

	return (phandle);
}

static void
set_single_reg(void *fdt, uint64_t start, uint64_t len)
{
	void *reg;

	fdt_property_placeholder(fdt, "reg", 2 * sizeof(uint64_t), &reg);
	SET_PROP_U64(reg, 0, start);
	SET_PROP_U64(reg, 1, len);
}

static void
add_cpu(void *fdt, int cpuid)
{
	char node_name[16];

	snprintf(node_name, sizeof(node_name), "cpu@%d", cpuid);

	fdt_begin_node(fdt, node_name);
	fdt_property_string(fdt, "device_type", "cpu");
	fdt_property_string(fdt, "compatible", "arm,armv8");
	fdt_property_u64(fdt, "reg", cpuid);
	fdt_property_string(fdt, "enable-method", "psci");
	fdt_end_node(fdt);
}

void
fdt_add_cpus(int ncpu)
{
	void *fdt;
	int cpuid;

	fdt = fdtroot;

	fdt_begin_node(fdt, "cpus");
	fdt_property_u32(fdt, "interrupt-parent", gic_phandle);
	/* XXX: Needed given the root #address-cells? */
	fdt_property_u32(fdt, "#address-cells", 2);
	fdt_property_u32(fdt, "#size-cells", 0);

	for (cpuid = 0; cpuid < ncpu; cpuid++) {
		add_cpu(fdt, cpuid);
	}
	fdt_end_node(fdt);
}

int
fdt_init(struct vmctx *ctx, int ncpu, void *fdtaddr, vm_size_t fdtsize)
{
	void *fdt;
	const char *bootargs;

	fdt = fdtaddr;
	if (fdt == NULL)
		return (EFAULT);

	fdt_create(fdt, (int)fdtsize);

	/* Add the memory reserve map (needed even if none is reserved) */
	fdt_finish_reservemap(fdt);

	/* Create the root node */
	fdt_begin_node(fdt, "");

	fdt_property_string(fdt, "compatible", "freebsd,bhyve");
	fdt_property_u32(fdt, "#address-cells", 2);
	fdt_property_u32(fdt, "#size-cells", 2);

	fdt_begin_node(fdt, "chosen");
	fdt_property_string(fdt, "stdout-path", "serial0:115200n8");
	bootargs = get_config_value("fdt.bootargs");
	if (bootargs != NULL)
		fdt_property_string(fdt, "bootargs", bootargs);
	fdt_end_node(fdt);

	fdt_begin_node(fdt, "memory");
	fdt_property_string(fdt, "device_type", "memory");
	/* There is no lowmem on arm64. */
	assert(vm_get_lowmem_size(ctx) == 0);
	set_single_reg(fdt, vm_get_highmem_base(ctx), vm_get_highmem_size(ctx));
	fdt_end_node(fdt);

	// add_cpus(fdt, ncpu);

	fdt_begin_node(fdt, "psci");
	fdt_property_string(fdt, "compatible", "arm,psci-0.2");
	fdt_property_string(fdt, "method", "hvc");
	fdt_end_node(fdt);

	fdt_begin_node(fdt, "apb-pclk");
	fdt_property_string(fdt, "compatible", "fixed-clock");
	fdt_property_string(fdt, "clock-output-names", "clk24mhz");
	fdt_property_u32(fdt, "#clock-cells", 0);
	fdt_property_u32(fdt, "clock-frequency", 24000000);
	apb_pclk_phandle = assign_phandle(fdt);
	fdt_end_node(fdt);

	/* Finalized by fdt_finalized(). */
	fdtroot = fdt;

	return (0);
}

void
fdt_add_gic(uint64_t dist_base, uint64_t dist_size, uint64_t redist_base,
    uint64_t redist_size, uint64_t mmio_base, int spi_intid_base, int spi_count)
{
	char node_name[32];
	void *fdt, *prop;

	fdt = fdtroot;

	snprintf(node_name, sizeof(node_name), "interrupt-controller@%lx",
	    (unsigned long)dist_base);
	fdt_begin_node(fdt, node_name);

	gic_phandle = assign_phandle(fdt);
	fdt_property_string(fdt, "compatible", "arm,gic-v3");
	fdt_property(fdt, "interrupt-controller", NULL, 0);
	if (spi_count)
		fdt_property(fdt, "msi-controller", NULL, 0);
	/* XXX: Needed given the root #address-cells? */
	fdt_property_u32(fdt, "#address-cells", 2);
	fdt_property_u32(fdt, "#interrupt-cells", 3);

	if (guest_ncpus > 1) {
		fdt_property_u32(fdt, "#redistributor-regions", guest_ncpus);
	}
	fdt_property_placeholder(fdt, "reg",
	    2 * sizeof(uint64_t) * (1 + guest_ncpus), &prop);
	/* GICD */
	SET_PROP_U64(prop, 0, dist_base);
	SET_PROP_U64(prop, 1, dist_size);
	/* GICR */
	for (int ncpu = 0; ncpu < guest_ncpus; ncpu++) {
		SET_PROP_U64(prop, 2 + ncpu * 2,
		    redist_base + ncpu * redist_size);
		SET_PROP_U64(prop, 3 + ncpu * 2, redist_size);
	}

	if (spi_count) {
		fdt_property_placeholder(fdt, "mbi-ranges",
		    2 * sizeof(uint32_t), &prop);
		SET_PROP_U32(prop, 0, spi_intid_base);
		SET_PROP_U32(prop, 1, spi_count);

		fdt_property_placeholder(fdt, "mbi-alias",
		    2 * sizeof(uint32_t), &prop);
		SET_PROP_U32(prop, 0, mmio_base >> 32);
		SET_PROP_U32(prop, 1, mmio_base);
	}

	fdt_end_node(fdt); // GIC

	fdt_property_u32(fdt, "interrupt-parent", gic_phandle);
}

void
fdt_add_uart(uint64_t uart_base, uint64_t uart_size, int intr)
{
	void *fdt, *interrupts, *prop;
	char node_name[32];

	assert(gic_phandle != 0);
	assert(apb_pclk_phandle != 0);
	assert(intr >= GIC_FIRST_SPI);

	fdt = fdtroot;

	snprintf(node_name, sizeof(node_name), "serial@%llx", uart_base);
	fdt_begin_node(fdt, node_name);
#define UART_COMPAT "arm,pl011\0arm,primecell"
	fdt_property(fdt, "compatible", UART_COMPAT, sizeof(UART_COMPAT));
#undef UART_COMPAT
	set_single_reg(fdt, uart_base, uart_size);
	fdt_property_u32(fdt, "interrupt-parent", gic_phandle);
	fdt_property_placeholder(fdt, "interrupts", 3 * sizeof(uint32_t),
	    &interrupts);
	SET_PROP_U32(interrupts, 0, GIC_SPI);
	SET_PROP_U32(interrupts, 1, intr - GIC_FIRST_SPI);
	SET_PROP_U32(interrupts, 2, IRQ_TYPE_LEVEL_HIGH);
	fdt_property_placeholder(fdt, "clocks", 2 * sizeof(uint32_t), &prop);
	SET_PROP_U32(prop, 0, apb_pclk_phandle);
	SET_PROP_U32(prop, 1, apb_pclk_phandle);
#define UART_CLK_NAMES "uartclk\0apb_pclk"
	fdt_property(fdt, "clock-names", UART_CLK_NAMES,
	    sizeof(UART_CLK_NAMES));
#undef UART_CLK_NAMES

	fdt_end_node(fdt);

	snprintf(node_name, sizeof(node_name), "/serial@%llx", uart_base);
	fdt_begin_node(fdt, "aliases");
	fdt_property_string(fdt, "serial0", node_name);
	fdt_end_node(fdt);
}

void
fdt_add_rtc(uint64_t rtc_base, uint64_t rtc_size, int intr)
{
	void *fdt, *interrupts, *prop;
	char node_name[32];

	assert(gic_phandle != 0);
	assert(apb_pclk_phandle != 0);
	assert(intr >= GIC_FIRST_SPI);

	fdt = fdtroot;

	snprintf(node_name, sizeof(node_name), "rtc@%llx", rtc_base);
	fdt_begin_node(fdt, node_name);
#define RTC_COMPAT "arm,pl031\0arm,primecell"
	fdt_property(fdt, "compatible", RTC_COMPAT, sizeof(RTC_COMPAT));
#undef RTC_COMPAT
	set_single_reg(fdt, rtc_base, rtc_size);
	fdt_property_u32(fdt, "interrupt-parent", gic_phandle);
	fdt_property_placeholder(fdt, "interrupts", 3 * sizeof(uint32_t),
	    &interrupts);
	SET_PROP_U32(interrupts, 0, GIC_SPI);
	SET_PROP_U32(interrupts, 1, intr - GIC_FIRST_SPI);
	SET_PROP_U32(interrupts, 2, IRQ_TYPE_LEVEL_HIGH);
	fdt_property_placeholder(fdt, "clocks", sizeof(uint32_t), &prop);
	SET_PROP_U32(prop, 0, apb_pclk_phandle);
	fdt_property_string(fdt, "clock-names", "apb_pclk");

	fdt_end_node(fdt);
}

void
fdt_add_fwcfg(uint64_t mmio_base, uint64_t mmio_size)
{
	void *fdt;
	char node_name[32];

	fdt = fdtroot;

	snprintf(node_name, sizeof(node_name), "fw-cfg@%llx", mmio_base);
	fdt_begin_node(fdt, node_name);
#define FWCFG_COMPAT "qemu,fw-cfg-mmio"
	fdt_property(fdt, "compatible", FWCFG_COMPAT, sizeof(FWCFG_COMPAT));
#undef FWCFG_COMPAT
	fdt_property_u32(fdt, "#address-cells", 2);
	fdt_property_u32(fdt, "#size-cells", 2);

	set_single_reg(fdt, mmio_base, mmio_size);

	fdt_end_node(fdt);
}

void
fdt_add_tpm(uint64_t tpm_base, uint64_t tpm_size, const char *tpm_interface)
{
	char node_name[32];
	void *fdt;

	fdt = fdtroot;

	snprintf(node_name, sizeof(node_name), "tpm@%llx",
	    (unsigned long long)tpm_base);
	fdt_begin_node(fdt, node_name);
	fdt_property_string(fdt, "compatible", "tcg,tpm-tis-mmio");
	fdt_property_string(fdt, "scorpi,tpm-interface", tpm_interface);
	set_single_reg(fdt, tpm_base, tpm_size);
	fdt_end_node(fdt);
}

void
fdt_add_timer(void)
{
	void *fdt, *interrupts;
	uint32_t irqs[] = { 13, 14, 11, 12 };

	assert(gic_phandle != 0);

	fdt = fdtroot;

	fdt_begin_node(fdt, "timer");
	fdt_property_string(fdt, "compatible", "arm,armv8-timer");
	fdt_property_u32(fdt, "interrupt-parent", gic_phandle);
	fdt_property_placeholder(fdt, "interrupts", 12 * sizeof(uint32_t),
	    &interrupts);
	for (u_int i = 0; i < nitems(irqs); i++) {
		SET_PROP_U32(interrupts, i * 3 + 0, GIC_PPI);
		SET_PROP_U32(interrupts, i * 3 + 1, irqs[i]);
		SET_PROP_U32(interrupts, i * 3 + 2, IRQ_TYPE_LEVEL_LOW);
	}
	fdt_end_node(fdt);
}

struct lintr_cb {
	void *prop;
	const int *intrs;
	int cnt;
};

static void
lintr_cb(int b, int s, int pin, struct pci_irq *irq, void *arg)
{
	struct lintr_cb *p = (struct lintr_cb *)arg;
	int slot = s;
	void *prop = p->prop;
	int i = p->cnt++;
	int intr = p->intrs[(pin + slot - 1) % 4];

	assert(intr >= GIC_FIRST_SPI);
	SET_PROP_U32(prop, 10 * i + 0, slot << 11);
	SET_PROP_U32(prop, 10 * i + 1, 0);
	SET_PROP_U32(prop, 10 * i + 2, 0);
	SET_PROP_U32(prop, 10 * i + 3, pin);
	SET_PROP_U32(prop, 10 * i + 4, gic_phandle);
	SET_PROP_U32(prop, 10 * i + 5, 0);
	SET_PROP_U32(prop, 10 * i + 6, 0);
	SET_PROP_U32(prop, 10 * i + 7, GIC_SPI);
	SET_PROP_U32(prop, 10 * i + 8, intr - GIC_FIRST_SPI);
	SET_PROP_U32(prop, 10 * i + 9, IRQ_TYPE_LEVEL_HIGH);
}

void
fdt_add_pcie(int intrs[static 4], uint64_t iobase, uint64_t iolimit,
    uint64_t mmio_base32, uint64_t mmio_base_limit)
{
	void *fdt, *prop;
	struct lintr_cb p = { NULL, intrs, 0 };

	assert(gic_phandle != 0);

	fdt = fdtroot;

	fdt_begin_node(fdt, "pcie@1f0000000");
	fdt_property_string(fdt, "compatible", "pci-host-ecam-generic");
	fdt_property_u32(fdt, "#address-cells", 3);
	fdt_property_u32(fdt, "#size-cells", 2);
	fdt_property_string(fdt, "device_type", "pci");
	fdt_property_u64(fdt, "bus-range", (0ul << 32) | 1);
	set_single_reg(fdt, 0xe0000000, 0x10000000);
	fdt_property_placeholder(fdt, "ranges", 2 * 7 * sizeof(uint32_t),
	    &prop);
	SET_PROP_U32(prop, 0, 0x01000000);

	SET_PROP_U32(prop, 1, 0);
	SET_PROP_U32(prop, 2, iobase);

	SET_PROP_U32(prop, 3, 0);
	SET_PROP_U32(prop, 4, iobase);

	SET_PROP_U32(prop, 5, 0);
	SET_PROP_U32(prop, 6, iolimit - iobase);

	SET_PROP_U32(prop, 7, 0x02000000);

	SET_PROP_U32(prop, 8, 0);
	SET_PROP_U32(prop, 9, mmio_base32);

	SET_PROP_U32(prop, 10, 0);
	SET_PROP_U32(prop, 11, mmio_base32);

	SET_PROP_U32(prop, 12, 0);
	SET_PROP_U32(prop, 13, mmio_base_limit - mmio_base32);

	if (msi_supported() || msix_supported()) {
		fdt_property_placeholder(fdt, "msi-map",
		    4 * sizeof(uint32_t), &prop);
		SET_PROP_U32(prop, 0, 0); /* RID base */
		SET_PROP_U32(prop, 1, gic_phandle);
		SET_PROP_U32(prop, 2, 0); /* MSI base */
		SET_PROP_U32(prop, 3, 0x10000); /* RID length */
		fdt_property_u32(fdt, "msi-parent", gic_phandle);
	}
	fdt_property_u32(fdt, "#interrupt-cells", 1);
	fdt_property_u32(fdt, "interrupt-parent", gic_phandle);

	/*
	 * Describe standard swizzled interrupts routing (pins rotated by one
	 * for each consecutive slot). Must match pci_irq_route().
	 */
	fdt_property_placeholder(fdt, "interrupt-map-mask",
	    4 * sizeof(uint32_t), &prop);
	SET_PROP_U32(prop, 0, 0x1f << 11);
	SET_PROP_U32(prop, 1, 0);
	SET_PROP_U32(prop, 2, 0);
	SET_PROP_U32(prop, 3, 7);
	fdt_property_placeholder(fdt, "interrupt-map",
	    pci_count_lintr(0) * 10 * sizeof(uint32_t), &prop);

	p.prop = prop;
	pci_walk_lintr(0, lintr_cb, &p);

	fdt_end_node(fdt);
}

void
fdt_add_flash(uint64_t base, uint64_t size)
{
	void *fdt;
	char node_name[32];

	fdt = fdtroot;

	snprintf(node_name, sizeof(node_name), "flash@%llx", base);
	fdt_begin_node(fdt, node_name);
#define CFI_COMPAT "cfi-flash"
	fdt_property(fdt, "compatible", CFI_COMPAT, sizeof(CFI_COMPAT));
#undef CFI_COMPAT

	fdt_property_u32(fdt, "#address-cells", 1);
	fdt_property_u32(fdt, "#size-cells", 1);

	set_single_reg(fdt, base, size);

	fdt_end_node(fdt);
}

void
fdt_finalize(void)
{
	fdt_end_node(fdtroot);

	fdt_finish(fdtroot);
}
