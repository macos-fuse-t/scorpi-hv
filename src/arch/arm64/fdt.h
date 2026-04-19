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

#ifndef _FDT_H_
#define	_FDT_H_

#include <sys/types.h>
#include "common.h"

struct vmctx;

int	fdt_init(struct vmctx *ctx, int ncpu, void *addrp,
	    vm_size_t size);
void fdt_add_cpus(int ncpu);
void fdt_add_gic(uint64_t dist_base, uint64_t dist_size,
    uint64_t redist_base, uint64_t redist_size, 
	uint64_t mmio_base, int spi_intid_base, int spi_count);
void fdt_add_timer(void);
void fdt_add_pcie(int intrs[static 4], uint64_t iobase, uint64_t iolimit, 
			uint64_t mmio_base32, uint64_t mmio_base_limit);
void fdt_add_uart(uint64_t uart_base, uint64_t uart_size, int intr);
void fdt_add_rtc(uint64_t rtc_base, uint64_t rtc_size, int intr);
void fdt_add_fwcfg(uint64_t mmio_base, uint64_t mmio_size);
void fdt_add_tpm(uint64_t tpm_base, uint64_t tpm_size);
void fdt_add_flash(uint64_t base, uint64_t size);
void fdt_finalize(void);

#endif	/* _FDT_H_ */
