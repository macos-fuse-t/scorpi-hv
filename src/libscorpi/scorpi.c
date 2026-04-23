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

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "scorpi_internal.h"

enum scorpi_bus_kind {
	SCORPI_BUS_PCI = 0,
	SCORPI_BUS_USB,
	SCORPI_BUS_LPC,
};

static void
scorpi_prop_destroy(struct scorpi_prop *prop)
{
	if (prop == NULL)
		return;
	free(prop->name);
	if (prop->kind == SCORPI_PROP_STRING)
		free(prop->value.string);
	free(prop);
}

const struct scorpi_prop *
scorpi_vm_find_prop(const scorpi_vm_t *vm, const char *name)
{
	struct scorpi_prop *prop;

	if (vm == NULL || name == NULL)
		return (NULL);

	for (prop = vm->props; prop != NULL; prop = prop->next) {
		if (strcmp(prop->name, name) == 0)
			return (prop);
	}

	return (NULL);
}

static struct scorpi_prop *
scorpi_vm_find_prop_mut(scorpi_vm_t *vm, const char *name);

static struct scorpi_prop *
scorpi_vm_find_prop_mut(scorpi_vm_t *vm, const char *name)
{
	struct scorpi_prop *prop;

	assert(vm != NULL);
	assert(name != NULL);

	for (prop = vm->props; prop != NULL; prop = prop->next) {
		if (strcmp(prop->name, name) == 0)
			return (prop);
	}

	return (NULL);
}

static scorpi_error_t
scorpi_vm_ensure_prop(scorpi_vm_t *vm, const char *name,
    struct scorpi_prop **out_prop)
{
	struct scorpi_prop *prop;

	prop = scorpi_vm_find_prop_mut(vm, name);
	if (prop != NULL) {
		if (prop->kind == SCORPI_PROP_STRING) {
			free(prop->value.string);
			prop->value.string = NULL;
		}
		*out_prop = prop;
		return (SCORPI_OK);
	}

	prop = calloc(1, sizeof(*prop));
	if (prop == NULL)
		return (SCORPI_ERR_RUNTIME);

	prop->name = strdup(name);
	if (prop->name == NULL) {
		free(prop);
		return (SCORPI_ERR_RUNTIME);
	}

	prop->next = vm->props;
	vm->props = prop;
	*out_prop = prop;
	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_create_device_common(const char *device, uint64_t slot, int bus,
    scorpi_device_t **out_dev)
{
	scorpi_device_t *dev;

	if (device == NULL || *device == '\0' || out_dev == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	*out_dev = NULL;

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL)
		return (SCORPI_ERR_RUNTIME);

	dev->device = strdup(device);
	if (dev->device == NULL) {
		free(dev);
		return (SCORPI_ERR_RUNTIME);
	}

	dev->slot = slot;
	dev->bus = bus;
	*out_dev = dev;
	return (SCORPI_OK);
}

scorpi_error_t
scorpi_create_vm(scorpi_vm_t **out_vm)
{
	scorpi_vm_t *vm;

	if (out_vm == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	*out_vm = NULL;

	vm = calloc(1, sizeof(*vm));
	if (vm == NULL)
		return (SCORPI_ERR_RUNTIME);

	*out_vm = vm;
	return (SCORPI_OK);
}

void
scorpi_destroy_vm(scorpi_vm_t *vm)
{
	struct scorpi_prop *prop, *next;

	if (vm == NULL)
		return;

	for (prop = vm->props; prop != NULL; prop = next) {
		next = prop->next;
		scorpi_prop_destroy(prop);
	}
	free(vm);
}

scorpi_error_t
scorpi_vm_set_prop_string(scorpi_vm_t *vm, const char *name, const char *value)
{
	struct scorpi_prop *prop;
	scorpi_error_t error;

	if (vm == NULL || name == NULL || *name == '\0' || value == NULL)
		return (SCORPI_ERR_INVALID_ARG);

	error = scorpi_vm_ensure_prop(vm, name, &prop);
	if (error != SCORPI_OK)
		return (error);

	prop->kind = SCORPI_PROP_STRING;
	prop->value.string = strdup(value);
	if (prop->value.string == NULL)
		return (SCORPI_ERR_RUNTIME);

	return (SCORPI_OK);
}

scorpi_error_t
scorpi_vm_set_prop_bool(scorpi_vm_t *vm, const char *name, bool value)
{
	struct scorpi_prop *prop;
	scorpi_error_t error;

	(void)value;
	if (vm == NULL || name == NULL || *name == '\0')
		return (SCORPI_ERR_INVALID_ARG);

	error = scorpi_vm_ensure_prop(vm, name, &prop);
	if (error != SCORPI_OK)
		return (error);

	prop->kind = SCORPI_PROP_BOOL;
	prop->value.boolean = value;
	return (SCORPI_OK);
}

scorpi_error_t
scorpi_vm_set_prop_u64(scorpi_vm_t *vm, const char *name, uint64_t value)
{
	struct scorpi_prop *prop;
	scorpi_error_t error;

	(void)value;
	if (vm == NULL || name == NULL || *name == '\0')
		return (SCORPI_ERR_INVALID_ARG);

	error = scorpi_vm_ensure_prop(vm, name, &prop);
	if (error != SCORPI_OK)
		return (error);

	prop->kind = SCORPI_PROP_U64;
	prop->value.u64 = value;
	return (SCORPI_OK);
}

scorpi_error_t
scorpi_vm_set_cpu(scorpi_vm_t *vm, uint64_t cores)
{
	if (vm == NULL || cores == 0)
		return (SCORPI_ERR_INVALID_ARG);
	return (scorpi_vm_set_prop_u64(vm, "cpu.cores", cores));
}

scorpi_error_t
scorpi_vm_set_ram(scorpi_vm_t *vm, uint64_t bytes)
{
	if (vm == NULL || bytes == 0)
		return (SCORPI_ERR_INVALID_ARG);
	return (scorpi_vm_set_prop_u64(vm, "memory.size", bytes));
}

scorpi_error_t
scorpi_create_pci_device(const char *device, uint64_t slot,
    scorpi_device_t **out_dev)
{
	return (scorpi_create_device_common(device, slot, SCORPI_BUS_PCI,
	    out_dev));
}

scorpi_error_t
scorpi_create_usb_device(const char *device, scorpi_device_t **out_dev)
{
	return (scorpi_create_device_common(device, SCORPI_PCI_SLOT_AUTO,
	    SCORPI_BUS_USB, out_dev));
}

scorpi_error_t
scorpi_create_lpc_device(const char *device, scorpi_device_t **out_dev)
{
	return (scorpi_create_device_common(device, SCORPI_PCI_SLOT_AUTO,
	    SCORPI_BUS_LPC, out_dev));
}

void
scorpi_destroy_device(scorpi_device_t *dev)
{
	if (dev == NULL)
		return;
	free(dev->device);
	free(dev);
}

scorpi_error_t
scorpi_device_set_prop_string(scorpi_device_t *dev, const char *name,
    const char *value)
{
	if (dev == NULL || name == NULL || *name == '\0' || value == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	return (SCORPI_ERR_UNSUPPORTED);
}

scorpi_error_t
scorpi_device_set_prop_bool(scorpi_device_t *dev, const char *name, bool value)
{
	(void)value;
	if (dev == NULL || name == NULL || *name == '\0')
		return (SCORPI_ERR_INVALID_ARG);
	return (SCORPI_ERR_UNSUPPORTED);
}

scorpi_error_t
scorpi_device_set_prop_u64(scorpi_device_t *dev, const char *name,
    uint64_t value)
{
	(void)value;
	if (dev == NULL || name == NULL || *name == '\0')
		return (SCORPI_ERR_INVALID_ARG);
	return (SCORPI_ERR_UNSUPPORTED);
}

scorpi_error_t
scorpi_vm_add_device(scorpi_vm_t *vm, scorpi_device_t *dev)
{
	if (vm == NULL || dev == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	return (SCORPI_ERR_UNSUPPORTED);
}

scorpi_error_t
scorpi_start_vm(scorpi_vm_t *vm)
{
	if (vm == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	return (SCORPI_ERR_UNSUPPORTED);
}

scorpi_error_t
scorpi_wait_vm(scorpi_vm_t *vm, int *exit_code)
{
	if (vm == NULL || exit_code == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	return (SCORPI_ERR_UNSUPPORTED);
}

scorpi_error_t
scorpi_stop_vm(scorpi_vm_t *vm)
{
	if (vm == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	return (SCORPI_ERR_UNSUPPORTED);
}

scorpi_error_t
scorpi_load_vm_from_yaml(const char *yaml, scorpi_vm_t **out_vm)
{
	if (yaml == NULL || out_vm == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	*out_vm = NULL;
	return (SCORPI_ERR_UNSUPPORTED);
}
