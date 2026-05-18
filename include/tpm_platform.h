/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

/*
 * On PC/x86 guests, Linux treats the low RAM area as already owned and will
 * reject an ACPI TPM resource there. Use the conventional TPM MMIO window.
 */
#if defined(__x86_64__)
#define TPM_MMIO_ADDRESS 0xfed40000
#else
#define TPM_MMIO_ADDRESS 0x20000
#endif
