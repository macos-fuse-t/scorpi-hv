/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2022 Beckhoff Automation GmbH & Co. KG
 * Author: Corvin Kohne <c.koehne@beckhoff.com>
 */

#include <sys/types.h>
#include <support/endian.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "mem.h"
#include "qemu_fwcfg.h"
#include "tpm_ppi.h"

#define TPM_PPI_ADDRESS 0x25000
#define TPM_PPI_SIZE 0x400

#define TPM_PPI_FWCFG_FILE "etc/tpm/config"

#define TPM_PPI_QEMU_NAME "qemu"

struct tpm_ppi_qemu {
	uint8_t func[256];	    /* FUNC */
	uint8_t in;		    /* PPIN */
	uint32_t ip;		    /* PPIP */
	uint32_t response;	    /* PPRP */
	uint32_t request;	    /* PPRQ */
	uint32_t request_parameter; /* PPRM */
	uint32_t last_request;	    /* LPPR */
	uint32_t func_ret;	    /* FRET */
	uint8_t _reserved1[0x40];   /* RES1 */
	uint8_t next_step;	    /* next_step */
} __attribute__((packed));
static_assert(sizeof(struct tpm_ppi_qemu) <= TPM_PPI_SIZE,
    "Wrong size of tpm_ppi_qemu");

struct tpm_ppi_fwcfg {
	uint32_t ppi_address;
	uint8_t tpm_version;
	uint8_t ppi_version;
} __attribute__((packed));

static int
tpm_ppi_mem_handler(struct vcpu *const vcpu __unused, const int dir,
    const uint64_t addr, const int size, uint64_t *const val, void *const arg1,
    const long arg2 __unused)
{
	struct tpm_ppi_qemu *ppi;
	uint8_t *ptr;
	uint64_t off;

	if ((addr & (size - 1)) != 0) {
		warnx("%s: unaligned %s access @ %#llx [size = %x]", __func__,
		    dir == MEM_F_READ ? "read" : "write",
		    (unsigned long long)addr, size);
		return (EINVAL);
	}

	if (!(size == 1 || size == 2 || size == 4 || size == 8))
		return (EINVAL);

	ppi = arg1;
	off = addr - TPM_PPI_ADDRESS;
	if (off >= TPM_PPI_SIZE || off + size > TPM_PPI_SIZE)
		return (EINVAL);

	ptr = (uint8_t *)ppi + off;
	if (dir == MEM_F_READ) {
		*val = 0;
		memcpy(val, ptr, size);
	} else {
		memcpy(ptr, val, size);
	}

	return (0);
}

static struct mem_range ppi_mmio = {
	.name = "ppi-mmio",
	.base = TPM_PPI_ADDRESS,
	.size = TPM_PPI_SIZE,
	.flags = MEM_F_RW,
	.handler = tpm_ppi_mem_handler,
};

static int
tpm_ppi_init(void **sc)
{
	struct tpm_ppi_qemu *ppi;
	struct tpm_ppi_fwcfg *fwcfg;
	int error;

	ppi = calloc(1, TPM_PPI_SIZE);
	if (ppi == NULL) {
		warnx("%s: failed to allocate ACPI region for PPI", __func__);
		return (ENOMEM);
	}

	fwcfg = calloc(1, sizeof(*fwcfg));
	if (fwcfg == NULL) {
		warnx("%s: failed to allocate fw_cfg item", __func__);
		error = ENOMEM;
		goto err_out;
	}

	fwcfg->ppi_address = htole32(TPM_PPI_ADDRESS);
	fwcfg->tpm_version = 2;
	fwcfg->ppi_version = 1;

	error = qemu_fwcfg_add_file_deferred(TPM_PPI_FWCFG_FILE,
	    sizeof(*fwcfg), fwcfg);
	if (error) {
		warnc(error, "%s: failed to add fw_cfg file", __func__);
		free(fwcfg);
		goto err_out;
	}

	ppi_mmio.arg1 = ppi;
	error = register_mem(&ppi_mmio);
	if (error) {
		warnc(error, "%s: failed to register PPI MMIO", __func__);
		goto err_out;
	}

	*sc = ppi;
	return (0);

err_out:
	free(ppi);
	return (error);
}

static void
tpm_ppi_deinit(void *sc)
{
	int error;

	if (sc == NULL)
		return;

	error = unregister_mem(&ppi_mmio);
	assert(error == 0);
	free(sc);
}

static struct tpm_ppi tpm_ppi_qemu = {
	.name = TPM_PPI_QEMU_NAME,
	.init = tpm_ppi_init,
	.deinit = tpm_ppi_deinit,
};
TPM_PPI_SET(tpm_ppi_qemu);
