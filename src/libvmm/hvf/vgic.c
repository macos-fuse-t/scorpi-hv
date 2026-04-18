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

#include <sys/param.h>
#include <Hypervisor/Hypervisor.h>

#include <sys/mman.h>
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#include "vmm.h"
#include "vmmapi.h"

#include "config.h"
#include "debug.h"
#include "mem.h"
#include "mevent.h"

#define TYPER_MBIS (1 << 16)
#define GICD_IIDR 0x0008
#define GICD_TYPER2 0x000c
#define GICD_STATUSR 0x0010

struct vgic_softc {
	struct mem_range mr;
	uintptr_t shadow_dist;
};

void
vgic_write(struct vgic_softc *sc, int offset, uint32_t value)
{
	switch (offset) {
	case HV_GIC_DISTRIBUTOR_REG_GICD_CTLR:
		// if we don't do this it crashes and types: "FIXME IF: "Handle
		// this" line 1479" Would you Apple?
		value &= 0x8000007F;
		break;
	case 0x40: // GICD_SETSPI_NSR
		EPRINTLN("!!!!SPI %x\n", value);
		break;
	}
	if (hv_gic_set_distributor_reg(offset, value) != HV_SUCCESS) {
		EPRINTLN("hv_gic_set_distributor_reg() failed: offset 0x%x\n",
		    offset);
	}
}

uint32_t
vgic_read(struct vgic_softc *sc, int offset)
{
	uint64_t val;

	switch (offset) {
	case GICD_IIDR:
	case GICD_TYPER2:
	case GICD_STATUSR:
		return (0);
	}

	if (hv_gic_get_distributor_reg(offset, &val) != HV_SUCCESS) {
		EPRINTLN("hv_gic_get_distributor_reg() failed: offset 0x%x\n",
		    offset);
		return 0;
	}

	switch (offset) {
	case HV_GIC_DISTRIBUTOR_REG_GICD_TYPER:
		val |= TYPER_MBIS;
		break;
	}
	return val;
}

static int
mmio_vgic_mem_handler(struct vcpu *vcpu __unused, int dir, uint64_t addr,
    int size __unused, uint64_t *val, void *arg1, long arg2)
{
	struct vgic_softc *sc = arg1;
	long reg;

	reg = addr - arg2;
	if (dir == MEM_F_WRITE)
		vgic_write(sc, reg, *val);
	else
		*val = vgic_read(sc, reg);

	return (0);
}

int
init_apple_vgic(struct vmctx *ctx, uint64_t dist_start, size_t dist_size,
    uint64_t redist_start, size_t redist_size, uint64_t mmio_base,
    uint32_t spi_intid_base, uint32_t spi_intid_count)
{
	struct vgic_softc *sc;
	int error;
	uint64_t typer = 0;
	uint64_t dist_gpa;

	hv_gic_config_t gic_cfg;

	dist_gpa = spi_intid_count ? dist_start - dist_size : dist_start;

	gic_cfg = hv_gic_config_create();
	if (hv_gic_config_set_distributor_base(gic_cfg, dist_gpa) !=
	    HV_SUCCESS) {
		EPRINTLN("hv_gic_config_set_distributor_base() failed");
		return (-1);
	}
	if (hv_gic_config_set_redistributor_base(gic_cfg, redist_start) !=
	    HV_SUCCESS) {
		EPRINTLN("hv_gic_config_set_redistributor_base() failed");
		return (-1);
	}

	if (spi_intid_count) {
		if (hv_gic_config_set_msi_region_base(gic_cfg, mmio_base) !=
		    HV_SUCCESS) {
			EPRINTLN("hv_gic_config_set_msi_region_base() failed");
			return (-1);
		}
		if (hv_gic_config_set_msi_interrupt_range(gic_cfg,
			spi_intid_base, spi_intid_count) != HV_SUCCESS) {
			EPRINTLN(
			    "hv_gic_config_set_msi_interrupt_range() failed");
			return (-1);
		}
	}

	if (hv_gic_create(gic_cfg) != HV_SUCCESS) {
		EPRINTLN("hv_gic_create() failed");
		return (-1);
	}

	// MSI not supported
	if (!spi_intid_count) {
		return (0);
	}

	hv_gic_get_distributor_reg(HV_GIC_DISTRIBUTOR_REG_GICD_TYPER, &typer);
	if (!spi_intid_count || (typer & TYPER_MBIS)) {
		return (0);
	}

	// This is an ugly workaround to enale MSI interrupt. For some reason
	// TYPER is not reporting MBIS bit effectively disabling MSIs. Why is
	// this Apple? We create a shadow distributor page to fix that.
	sc = calloc(1, sizeof(struct vgic_softc));
	bzero(&sc->mr, sizeof(struct mem_range));

	sc->shadow_dist = 0;
	error = vm_setup_memory_segment(ctx, dist_gpa, dist_size,
	    PROT_READ | PROT_WRITE, &sc->shadow_dist);
	if (error) {
		free(sc);
		return (-1);
	}

	sc->mr.name = "vgic_distributor";
	sc->mr.base = dist_start;
	sc->mr.size = dist_size;
	sc->mr.flags = MEM_F_RW;
	sc->mr.handler = mmio_vgic_mem_handler;
	sc->mr.arg1 = sc;
	sc->mr.arg2 = sc->mr.base;
	error = register_mem(&sc->mr);
	assert(error == 0);

	return (0);
}
