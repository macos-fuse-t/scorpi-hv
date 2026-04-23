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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct scorpi_vm scorpi_vm_t;
typedef struct scorpi_device scorpi_device_t;

typedef enum scorpi_error {
	SCORPI_OK = 0,
	SCORPI_ERROR = 1,
} scorpi_error_t;

#define SCORPI_PCI_SLOT_AUTO UINT64_MAX

scorpi_error_t scorpi_create_vm(scorpi_vm_t **out_vm);
void scorpi_destroy_vm(scorpi_vm_t *vm);

scorpi_error_t scorpi_vm_set_prop_string(scorpi_vm_t *vm, const char *name,
    const char *value);
scorpi_error_t scorpi_vm_set_prop_bool(scorpi_vm_t *vm, const char *name,
    bool value);
scorpi_error_t scorpi_vm_set_prop_u64(scorpi_vm_t *vm, const char *name,
    uint64_t value);
scorpi_error_t scorpi_vm_set_cpu(scorpi_vm_t *vm, uint64_t cores);
scorpi_error_t scorpi_vm_set_ram(scorpi_vm_t *vm, uint64_t bytes);

scorpi_error_t scorpi_create_pci_device(const char *device, uint64_t slot,
    scorpi_device_t **out_dev);
scorpi_error_t scorpi_create_usb_device(const char *device,
    scorpi_device_t **out_dev);
scorpi_error_t scorpi_create_lpc_device(const char *device,
    scorpi_device_t **out_dev);
void scorpi_destroy_device(scorpi_device_t *dev);

scorpi_error_t scorpi_device_set_prop_string(scorpi_device_t *dev,
    const char *name, const char *value);
scorpi_error_t scorpi_device_set_prop_bool(scorpi_device_t *dev,
    const char *name, bool value);
scorpi_error_t scorpi_device_set_prop_u64(scorpi_device_t *dev,
    const char *name, uint64_t value);
scorpi_error_t scorpi_vm_add_device(scorpi_vm_t *vm, scorpi_device_t *dev);

scorpi_error_t scorpi_start_vm(scorpi_vm_t *vm);
scorpi_error_t scorpi_wait_vm(scorpi_vm_t *vm, int *exit_code);
scorpi_error_t scorpi_stop_vm(scorpi_vm_t *vm);

scorpi_error_t scorpi_load_vm_from_yaml(const char *yaml, scorpi_vm_t **out_vm);

#ifdef __cplusplus
}
#endif
