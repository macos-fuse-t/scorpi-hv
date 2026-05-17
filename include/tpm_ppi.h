/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2022 Beckhoff Automation GmbH & Co. KG
 * Author: Corvin Köhne <c.koehne@beckhoff.com>
 */

#pragma once

#include "config.h"

#define	TPM_PPI_QEMU_ADDRESS	0x25000
#define	TPM_PPI_QEMU_SIZE	0x400

struct tpm_ppi {
	const char *name;

	int (*init)(void **sc);
	void (*deinit)(void *sc);
	int (*write_dsdt_regions)(void *sc);
	int (*write_dsdt_dsm)(void *sc);
};
#define TPM_PPI_SET(x) DATA_SET(tpm_ppi_set, x)
