/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2015 Neel Natu <neel@freebsd.org>
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

#include <sys/types.h>
#include <sys/param.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "vmm.h"

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vmmapi.h>

#include "bootrom_arch.h"
#include "bhyverun.h"
#include "bootrom.h"
#include "cfi_reg.h"
#include "debug.h"
#include "mem.h"

#define BOOTROM_SIZE 0x4000000

#define _PROT_ALL    (PROT_READ | PROT_WRITE | PROT_EXEC)
#define CFI_DUAL_STATUS(status) (((status) << 16) | (status))

static int debug = 0;
#define DPRINTF(params) \
	if (debug)      \
	PRINTLN params
#define WPRINTF(params) PRINTLN params

/*
 * ROM region is 16 MB at the top of 4GB ("low") memory.
 *
 * The size is limited so it doesn't encroach into reserved MMIO space (e.g.,
 * APIC, HPET, MSI).
 *
 * It is allocated in page-multiple blocks on a first-come first-serve basis,
 * from high to low, during initialization, and does not change at runtime.
 */
static char *romptr;	    /* Pointer to userspace-mapped bootrom region. */
static vm_paddr_t gpa_base; /* GPA of low end of region. */
static vm_paddr_t gpa_allocbot; /* Low GPA of free region. */
static vm_paddr_t gpa_alloctop; /* High GPA, minus 1, of free region. */
static vm_paddr_t rom_gpa;
static size_t rom_len;

typedef enum read_state {
	CFI_STATE_READ_ARRAY,
	CFI_STATE_READ_STATUS,
	CFI_STATE_READ_ID,
} read_state;

typedef enum write_state {
	CFI_STATE_WRITE_IDLE,
	CFI_STATE_WRITE_BYTE,
	CFI_STATE_WRITE_PROG_LEN,
	CFI_STATE_WRITE_ERASE_CONFIRM,
	CFI_STATE_WRITE_CONFIRM,
} write_state;

static struct bootrom_var_state {
	uint8_t *mmap;
	struct vmctx *ctx;
	uint64_t gpa;
	off_t size;
	uint32_t status;
	read_state read_state;
	write_state write_state;
	size_t block_size;
	size_t prog_len;
	bool memslot_visible;
} var = { NULL, NULL, 0, 0, CFI_DUAL_STATUS(CFI_INTEL_STATUS_WSMS),
			CFI_STATE_READ_ARRAY, CFI_STATE_WRITE_IDLE,
			BOOTROM_VAR_BLOCK_SIZE, 0, false };

static int
bootrom_var_set_memslot_visible(bool visible)
{
	int error;

	if (var.ctx == NULL || var.size == 0 || var.memslot_visible == visible)
		return (0);

	error = vm_set_memory_segment_visible(var.ctx, var.gpa, var.size,
	    visible);
	if (error != 0)
		return (error);
	var.memslot_visible = visible;
	return (0);
}

static void
bootrom_var_trap_reads(void)
{
	int error;

	error = bootrom_var_set_memslot_visible(false);
	if (error != 0)
		EPRINTLN("bootrom vars: failed to trap reads: %s",
		    strerror(error));
}

static void
bootrom_var_fast_reads(void)
{
	int error;

	error = bootrom_var_set_memslot_visible(true);
	if (error != 0)
		EPRINTLN("bootrom vars: failed to restore fast reads: %s",
		    strerror(error));
}

void
bootrom_reset(void)
{
	var.status = CFI_DUAL_STATUS(CFI_INTEL_STATUS_WSMS);
	var.read_state = CFI_STATE_READ_ARRAY;
	var.write_state = CFI_STATE_WRITE_IDLE;
	var.prog_len = 0;
	bootrom_var_fast_reads();
}

static uint64_t
bootrom_var_mem_read(struct vcpu *vcpu __unused, uint64_t offset, int size)
{
	uint64_t val = 0;

	switch (var.read_state) {
	case CFI_STATE_READ_ARRAY:
		DPRINTF(("reading %llx", offset));
		memcpy(&val, var.mmap + offset, size);
		break;
	case CFI_STATE_READ_STATUS:
		val = var.status;
		break;
	case CFI_STATE_READ_ID:
		break;
	}
	return (val);
}

static void
bootrom_var_mem_write(struct vcpu *vcpu __unused, uint64_t offset, uint64_t val,
    int size)
{
	uint8_t cmd = val & 0xff;
	bool handled = false;

	switch (var.write_state) {
	case CFI_STATE_WRITE_BYTE:
		DPRINTF(("write byte: offset %llx, len %d, value %llx", offset,
		    size, val));
		memcpy(var.mmap + offset, &val, size);
		var.status = CFI_DUAL_STATUS(CFI_INTEL_STATUS_WSMS);
		var.write_state = CFI_STATE_WRITE_IDLE;
		var.read_state = CFI_STATE_READ_STATUS;
		handled = true;
		break;
	case CFI_STATE_WRITE_PROG_LEN:
		var.prog_len = (val & 0xFFFF) + 1;
		DPRINTF(("write prog len: %zx", var.prog_len));
		var.write_state = CFI_STATE_WRITE_CONFIRM;
		var.read_state = CFI_STATE_READ_STATUS;
		handled = true;
		break;
	case CFI_STATE_WRITE_ERASE_CONFIRM:
		if (cmd != CFI_BCS_CONFIRM)
			EPRINTLN(
			    "unexpected cmd %x in state CFI_STATE_WRITE_ERASE_CONFIRM",
			    cmd);

		DPRINTF(("erase confirm %llx", offset));
		memset(var.mmap + offset, 0xff, MIN(var.block_size,
		    (size_t)(var.size - offset)));
		var.status = CFI_DUAL_STATUS(CFI_INTEL_STATUS_WSMS);
		var.write_state = CFI_STATE_WRITE_IDLE;
		var.read_state = CFI_STATE_READ_STATUS;
		handled = true;
		break;
	case CFI_STATE_WRITE_CONFIRM:
		if (var.prog_len == 0) {
			if (cmd != CFI_BCS_CONFIRM) {
				EPRINTLN(
				    "unexpected write in state CFI_STATE_WRITE_CONFIRM %x, %llx, %lu",
				    cmd, offset, var.prog_len);
				break;
			}
			DPRINTF(("write confirm: offse %llx, len %lx", offset,
			    var.prog_len));
			var.write_state = CFI_STATE_WRITE_IDLE;
			var.read_state = CFI_STATE_READ_STATUS;
		} else {
			DPRINTF(("writing %d bytes at %llx (%llx)", size,
			    offset, val));
			assert(size == 4);
			memcpy(var.mmap + offset, &val, size);
			var.prog_len--;
		}
		handled = true;
		break;
	default:
		break;
	}

	if (handled)
		return;

	DPRINTF(("bootrom_var_mem_write: %x", cmd));
	switch (cmd) {
	case CFI_BCS_READ_STATUS:
		bootrom_var_trap_reads();
		var.read_state = CFI_STATE_READ_STATUS;
		break;
	case CFI_INTEL_READ_ID:
		bootrom_var_trap_reads();
		var.read_state = CFI_STATE_READ_ID;
		break;
	case CFI_BCS_CLEAR_STATUS:
		bootrom_var_trap_reads();
		var.status = 0;
		break;
	case CFI_BCS_WRITE_BYTE:
	case CFI_BCS_PROGRAM:
		bootrom_var_trap_reads();
		var.write_state = CFI_STATE_WRITE_BYTE;
		var.read_state = CFI_STATE_READ_STATUS;
		break;
	case CFI_BCS_BLOCK_ERASE:
		bootrom_var_trap_reads();
		DPRINTF(("block erase %llx", offset));
		var.status = CFI_DUAL_STATUS(CFI_INTEL_STATUS_WSMS);
		var.write_state = CFI_STATE_WRITE_ERASE_CONFIRM;
		var.read_state = CFI_STATE_READ_STATUS;
		break;
	case CFI_BCS_CONFIRM:
		break;
	case CFI_BCS_BUF_PROG_SETUP:
		bootrom_var_trap_reads();
		var.status = CFI_DUAL_STATUS(CFI_INTEL_STATUS_WSMS);
		var.write_state = CFI_STATE_WRITE_PROG_LEN;
		var.read_state = CFI_STATE_READ_STATUS;
		break;
	case CFI_BCS_READ_ARRAY:
	case CFI_BCS_READ_ARRAY2:
		DPRINTF(("read array"));
		var.read_state = CFI_STATE_READ_ARRAY;
		bootrom_var_fast_reads();
		break;
	default:
		EPRINTLN("bootrom_var_mem_write: unexpected write cmd %x", cmd);
		if (!cmd)
			exit(-1);
		break;
	}
}

/*
 * Emulate just those CFI basic commands that will convince EDK II
 * that the Firmware Volume area is writable and persistent.
 */
static int
bootrom_var_mem_handler(struct vcpu *vcpu __unused, int dir, uint64_t addr,
    int size, uint64_t *val, void *arg1 __unused, long arg2 __unused)
{
	off_t offset;
	uint64_t v;

	offset = addr - var.gpa;
	if (offset + size > var.size || offset < 0 || offset + size <= offset) {
		return (EINVAL);
	}

	if (dir == MEM_F_WRITE) {
		bootrom_var_mem_write(vcpu, offset, *val, size);
	} else {
		v = bootrom_var_mem_read(vcpu, offset, size);
		memcpy(val, &v, size);
	}
	return (0);
}

void
init_bootrom(struct vmctx *ctx)
{
	vm_paddr_t highmem;

	highmem = vm_get_highmem_base(ctx);
	gpa_base = highmem - BOOTROM_SIZE;
	gpa_allocbot = gpa_base;
	gpa_alloctop = highmem - 1;
}

int
bootrom_alloc(struct vmctx *ctx, size_t len, int prot, int flags,
    char **region_out, uint64_t *gpa_out)
{
	static const int bootrom_valid_flags = BOOTROM_ALLOC_TOP;

	vm_paddr_t gpa;

	if (flags & ~bootrom_valid_flags) {
		warnx("%s: Invalid flags: %x", __func__,
		    flags & ~bootrom_valid_flags);
		return (EINVAL);
	}
	if (prot & ~_PROT_ALL) {
		warnx("%s: Invalid protection: %x", __func__,
		    prot & ~_PROT_ALL);
		return (EINVAL);
	}

	if (len == 0 || len > BOOTROM_SIZE) {
		warnx("ROM size %zu is invalid", len);
		return (EINVAL);
	}
	if (len & PAGE_MASK) {
		warnx("ROM size %zu is not a multiple of the page size", len);
		return (EINVAL);
	}

	if (flags & BOOTROM_ALLOC_TOP) {
		gpa = (gpa_alloctop - len) + 1;
		if (gpa < gpa_allocbot) {
			warnx("No room for %zu ROM in bootrom region", len);
			return (ENOMEM);
		}
	} else {
		gpa = gpa_allocbot;
		if (gpa > (gpa_alloctop - len) + 1) {
			warnx("No room for %zu ROM in bootrom region", len);
			return (ENOMEM);
		}
	}

	if (vm_setup_bootrom_segment(ctx, gpa, len, (uintptr_t *)&romptr)) {
		err(4, "%s: vm_setup_bootrom_segment", __func__);
	}

	if (flags & BOOTROM_ALLOC_TOP)
		gpa_alloctop = gpa - 1;
	else
		gpa_allocbot = gpa + len;

	*region_out = romptr;
	if (gpa_out != NULL)
		*gpa_out = gpa;
	return (0);
}

int
bootrom_loadrom(struct vmctx *ctx)
{
	struct stat sbuf;
	off_t rom_size, var_size, total_size;
	size_t alloc_size;
	uint64_t alloc_gpa;
	char *ptr, *romfile;
	int fd, varfd, rv;
	const char *bootrom, *varfile;
	void *data;

	rv = -1;
	varfd = -1;

	bootrom = get_config_value("bootrom");
	if (bootrom == NULL) {
		return (0);
	}

	/*
	 * get_config_value_node may use a thread local buffer to return
	 * variables. So, when we query the second variable, the first variable
	 * might get overwritten. For that reason, the bootrom should be
	 * duplicated.
	 */
	romfile = strdup(bootrom);
	if (romfile == NULL) {
		return (-1);
	}

	fd = open(romfile, O_RDONLY);
	if (fd < 0) {
		EPRINTLN("Error opening bootrom \"%s\": %s", romfile,
		    strerror(errno));
		goto done;
	}

	if (fstat(fd, &sbuf) < 0) {
		EPRINTLN("Could not fstat bootrom file \"%s\": %s", romfile,
		    strerror(errno));
		goto done;
	}

	rom_size = sbuf.st_size;

	varfile = get_config_value("bootvars");
	var_size = 0;
	if (varfile != NULL) {
		varfd = open(varfile, O_RDWR);
		if (varfd < 0) {
			EPRINTLN("Error opening bootrom variable file "
				 "\"%s\": %s",
			    varfile, strerror(errno));
			goto done;
		}

		if (fstat(varfd, &sbuf) < 0) {
			EPRINTLN(
			    "Could not fstat bootrom variable file \"%s\": %s",
			    varfile, strerror(errno));
			goto done;
		}

		var_size = sbuf.st_size;
	}

	if (var_size > BOOTROM_SIZE ||
	    (var_size != 0 && var_size < PAGE_SIZE)) {
		EPRINTLN("Invalid bootrom variable size %lld", var_size);
		goto done;
	}

	total_size = rom_size + var_size;

	if (total_size > BOOTROM_SIZE) {
		EPRINTLN("Invalid bootrom and variable aggregate size %lld",
		    total_size);
		goto done;
	}
	alloc_size = (rom_size + PAGE_MASK) & ~PAGE_MASK;
	if (alloc_size == 0)
		alloc_size = PAGE_SIZE;

	/* Map the bootrom into the guest address space */
	if (bootrom_alloc(ctx, alloc_size, PROT_READ | PROT_EXEC,
		BOOTROM_ALLOC_TOP, &ptr, &alloc_gpa) != 0) {
		goto done;
	}
	rom_gpa = alloc_gpa;
	rom_len = alloc_size;

	/* Read 'romfile' into the guest address space. */
	data = mmap(NULL, rom_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (data == MAP_FAILED)
		err(1, "mmap(%s)", romfile);
	close(fd);
	bootrom_copyrom(ptr, alloc_size, data, rom_size);

	if (munmap(data, rom_size) != 0)
		err(1, "munmap(%s)", romfile);

	if (varfd >= 0) {
		var.mmap = mmap(NULL, var_size, PROT_READ | PROT_WRITE,
		    MAP_SHARED, varfd, 0);
		if (var.mmap == MAP_FAILED)
			goto done;
		var.size = var_size;
		var.gpa = (gpa_alloctop - var_size) + 1;
		var.ctx = ctx;
		DPRINTF(("bootrom vars: gpa %llx (%llx, size %llx)", var.gpa,
		    gpa_alloctop, var_size));
		gpa_alloctop = var.gpa - 1;
		uintptr_t var_addr = (uintptr_t)var.mmap;
		rv = vm_setup_memory_segment(ctx, var.gpa, var.size,
		    PROT_READ | PROT_DONT_ALLOCATE, &var_addr);
		if (rv != 0)
			goto done;
		var.memslot_visible = true;
		rv = register_mem(&(struct mem_range) {
		    .name = "bootrom variable",
		    .flags = MEM_F_RW,
		    .handler = bootrom_var_mem_handler,
		    .base = var.gpa,
		    .size = var.size,
		});
		if (rv != 0)
			goto done;
	}

	rv = 0;
done:
	if (varfd >= 0)
		close(varfd);
	free(romfile);
	return (rv);
}

int
bootrom_vars(uint64_t *addr, uint64_t *size)
{
	if (!var.gpa)
		return (-1);
	*addr = var.gpa;
	*size = var.size;
	return (0);
}

/*
 * Are we relying on a bootrom to initialize the guest's CPU context?
 */
bool
bootrom_boot(void)
{
	return (get_config_value("bootrom") != NULL);
}

char *
bootrom_romptr()
{
	return romptr;
}

uint64_t
bootrom_romsize()
{
	return (rom_len != 0 ? rom_len : BOOTROM_SIZE);
}

uint64_t
bootrom_rombase()
{
	return (rom_gpa != 0 ? rom_gpa : gpa_base);
}
