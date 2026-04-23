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

static void
scorpi_prop_list_destroy(struct scorpi_prop *props)
{
	struct scorpi_prop *next, *prop;

	for (prop = props; prop != NULL; prop = next) {
		next = prop->next;
		scorpi_prop_destroy(prop);
	}
}

static const struct scorpi_prop *
scorpi_prop_find(const struct scorpi_prop *props, const char *name)
{
	const struct scorpi_prop *prop;

	if (name == NULL)
		return (NULL);

	for (prop = props; prop != NULL; prop = prop->next) {
		if (strcmp(prop->name, name) == 0)
			return (prop);
	}

	return (NULL);
}

static struct scorpi_prop *
scorpi_prop_find_mut(struct scorpi_prop *props, const char *name)
{
	struct scorpi_prop *prop;

	assert(name != NULL);

	for (prop = props; prop != NULL; prop = prop->next) {
		if (strcmp(prop->name, name) == 0)
			return (prop);
	}

	return (NULL);
}

static scorpi_error_t
scorpi_prop_ensure(struct scorpi_prop **props, const char *name,
    struct scorpi_prop **out_prop)
{
	struct scorpi_prop *prop;

	assert(props != NULL);
	assert(name != NULL);
	assert(out_prop != NULL);

	prop = scorpi_prop_find_mut(*props, name);
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

	prop->next = *props;
	*props = prop;
	*out_prop = prop;
	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_prop_set_string(struct scorpi_prop **props, const char *name,
    const char *value)
{
	struct scorpi_prop *prop;
	scorpi_error_t error;

	error = scorpi_prop_ensure(props, name, &prop);
	if (error != SCORPI_OK)
		return (error);

	prop->kind = SCORPI_PROP_STRING;
	prop->value.string = strdup(value);
	if (prop->value.string == NULL)
		return (SCORPI_ERR_RUNTIME);

	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_prop_set_bool(struct scorpi_prop **props, const char *name, bool value)
{
	struct scorpi_prop *prop;
	scorpi_error_t error;

	error = scorpi_prop_ensure(props, name, &prop);
	if (error != SCORPI_OK)
		return (error);

	prop->kind = SCORPI_PROP_BOOL;
	prop->value.boolean = value;
	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_prop_set_u64(struct scorpi_prop **props, const char *name,
    uint64_t value)
{
	struct scorpi_prop *prop;
	scorpi_error_t error;

	error = scorpi_prop_ensure(props, name, &prop);
	if (error != SCORPI_OK)
		return (error);

	prop->kind = SCORPI_PROP_U64;
	prop->value.u64 = value;
	return (SCORPI_OK);
}

const struct scorpi_prop *
scorpi_vm_find_prop(const struct scorpi_vm *vm, const char *name)
{
	if (vm == NULL)
		return (NULL);
	return (scorpi_prop_find(vm->props, name));
}

const struct scorpi_prop *
scorpi_device_find_prop(const struct scorpi_device *dev, const char *name)
{
	if (dev == NULL)
		return (NULL);
	return (scorpi_prop_find(dev->props, name));
}

static const char *
scorpi_device_get_id(const struct scorpi_device *dev)
{
	const struct scorpi_prop *prop;

	prop = scorpi_device_find_prop(dev, "id");
	if (prop == NULL || prop->kind != SCORPI_PROP_STRING)
		return (NULL);
	return (prop->value.string);
}

const struct scorpi_device *
scorpi_vm_find_device_by_id(const struct scorpi_vm *vm, const char *id)
{
	const struct scorpi_device *dev;

	if (vm == NULL || id == NULL || *id == '\0')
		return (NULL);

	for (dev = vm->devices; dev != NULL; dev = dev->next) {
		const char *existing_id;

		existing_id = scorpi_device_get_id(dev);
		if (existing_id != NULL && strcmp(existing_id, id) == 0)
			return (dev);
	}

	return (NULL);
}

scorpi_error_t
scorpi_vm_resolve_parent(const struct scorpi_vm *vm, const struct scorpi_device *dev,
    const struct scorpi_device **out_parent)
{
	const struct scorpi_prop *parent_prop;
	const struct scorpi_device *parent;
	const char *device_id;
	const char *parent_id;

	if (vm == NULL || dev == NULL || out_parent == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	*out_parent = NULL;

	parent_prop = scorpi_device_find_prop(dev, "parent");
	if (parent_prop == NULL)
		return (SCORPI_OK);
	if (parent_prop->kind != SCORPI_PROP_STRING ||
	    parent_prop->value.string == NULL ||
	    *parent_prop->value.string == '\0')
		return (SCORPI_ERR_INVALID_PARENT);

	parent_id = parent_prop->value.string;
	device_id = scorpi_device_get_id(dev);
	if (device_id != NULL && strcmp(device_id, parent_id) == 0)
		return (SCORPI_ERR_INVALID_PARENT);

	parent = scorpi_vm_find_device_by_id(vm, parent_id);
	if (parent == NULL)
		return (SCORPI_ERR_INVALID_PARENT);

	*out_parent = parent;
	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_create_device_common(const char *device, uint64_t slot, int bus,
    scorpi_device_t *out_dev)
{
	scorpi_device_t dev;

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
scorpi_create_vm(scorpi_vm_t *out_vm)
{
	scorpi_vm_t vm;

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
scorpi_destroy_vm(scorpi_vm_t vm)
{
	scorpi_device_t dev, next_dev;

	if (vm == NULL)
		return;

	for (dev = vm->devices; dev != NULL; dev = next_dev) {
		next_dev = dev->next;
		dev->attached = false;
		scorpi_destroy_device(dev);
	}
	scorpi_prop_list_destroy(vm->props);
	free(vm);
}

scorpi_error_t
scorpi_vm_set_prop_string(scorpi_vm_t vm, const char *name, const char *value)
{
	if (vm == NULL || name == NULL || *name == '\0' || value == NULL)
		return (SCORPI_ERR_INVALID_ARG);

	return (scorpi_prop_set_string(&vm->props, name, value));
}

scorpi_error_t
scorpi_vm_set_prop_bool(scorpi_vm_t vm, const char *name, bool value)
{
	if (vm == NULL || name == NULL || *name == '\0')
		return (SCORPI_ERR_INVALID_ARG);

	return (scorpi_prop_set_bool(&vm->props, name, value));
}

scorpi_error_t
scorpi_vm_set_prop_u64(scorpi_vm_t vm, const char *name, uint64_t value)
{
	if (vm == NULL || name == NULL || *name == '\0')
		return (SCORPI_ERR_INVALID_ARG);

	return (scorpi_prop_set_u64(&vm->props, name, value));
}

scorpi_error_t
scorpi_vm_set_cpu(scorpi_vm_t vm, uint64_t cores)
{
	if (vm == NULL || cores == 0)
		return (SCORPI_ERR_INVALID_ARG);
	return (scorpi_vm_set_prop_u64(vm, "cpu.cores", cores));
}

scorpi_error_t
scorpi_vm_set_ram(scorpi_vm_t vm, uint64_t bytes)
{
	if (vm == NULL || bytes == 0)
		return (SCORPI_ERR_INVALID_ARG);
	return (scorpi_vm_set_prop_u64(vm, "memory.size", bytes));
}

scorpi_error_t
scorpi_create_pci_device(const char *device, uint64_t slot,
    scorpi_device_t *out_dev)
{
	return (scorpi_create_device_common(device, slot, SCORPI_BUS_PCI,
	    out_dev));
}

scorpi_error_t
scorpi_create_usb_device(const char *device, scorpi_device_t *out_dev)
{
	return (scorpi_create_device_common(device, SCORPI_PCI_SLOT_AUTO,
	    SCORPI_BUS_USB, out_dev));
}

scorpi_error_t
scorpi_create_lpc_device(const char *device, scorpi_device_t *out_dev)
{
	return (scorpi_create_device_common(device, SCORPI_PCI_SLOT_AUTO,
	    SCORPI_BUS_LPC, out_dev));
}

void
scorpi_destroy_device(scorpi_device_t dev)
{
	if (dev == NULL)
		return;
	if (dev->attached)
		return;
	free(dev->device);
	scorpi_prop_list_destroy(dev->props);
	free(dev);
}

scorpi_error_t
scorpi_device_set_prop_string(scorpi_device_t dev, const char *name,
    const char *value)
{
	if (dev == NULL || name == NULL || *name == '\0' || value == NULL)
		return (SCORPI_ERR_INVALID_ARG);

	return (scorpi_prop_set_string(&dev->props, name, value));
}

scorpi_error_t
scorpi_device_set_prop_bool(scorpi_device_t dev, const char *name, bool value)
{
	if (dev == NULL || name == NULL || *name == '\0')
		return (SCORPI_ERR_INVALID_ARG);

	return (scorpi_prop_set_bool(&dev->props, name, value));
}

scorpi_error_t
scorpi_device_set_prop_u64(scorpi_device_t dev, const char *name,
    uint64_t value)
{
	if (dev == NULL || name == NULL || *name == '\0')
		return (SCORPI_ERR_INVALID_ARG);

	return (scorpi_prop_set_u64(&dev->props, name, value));
}

scorpi_error_t
scorpi_vm_add_device(scorpi_vm_t vm, scorpi_device_t dev)
{
	const char *id;

	if (vm == NULL || dev == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	if (dev->attached)
		return (SCORPI_ERR_INVALID_ARG);

	id = scorpi_device_get_id(dev);
	if (id != NULL && scorpi_vm_find_device_by_id(vm, id) != NULL)
		return (SCORPI_ERR_DUPLICATE_ID);

	dev->next = vm->devices;
	dev->attached = true;
	vm->devices = dev;
	return (SCORPI_OK);
}

scorpi_error_t
scorpi_start_vm(scorpi_vm_t vm)
{
	if (vm == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	return (SCORPI_ERR_UNSUPPORTED);
}

scorpi_error_t
scorpi_wait_vm(scorpi_vm_t vm, int *exit_code)
{
	if (vm == NULL || exit_code == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	return (SCORPI_ERR_UNSUPPORTED);
}

scorpi_error_t
scorpi_stop_vm(scorpi_vm_t vm)
{
	if (vm == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	return (SCORPI_ERR_UNSUPPORTED);
}

scorpi_error_t
scorpi_load_vm_from_yaml(const char *yaml, scorpi_vm_t *out_vm)
{
	if (yaml == NULL || out_vm == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	*out_vm = NULL;
	return (SCORPI_ERR_UNSUPPORTED);
}
