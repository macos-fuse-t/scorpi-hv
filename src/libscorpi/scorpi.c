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
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <libutil.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <yaml.h>

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

static int
scorpi_string_compare_nullable(const char *lhs, const char *rhs)
{
	if (lhs == NULL && rhs == NULL)
		return (0);
	if (lhs == NULL)
		return (-1);
	if (rhs == NULL)
		return (1);
	return (strcmp(lhs, rhs));
}

static size_t
scorpi_prop_count(const struct scorpi_prop *props)
{
	size_t count;

	count = 0;
	for (; props != NULL; props = props->next)
		count++;
	return (count);
}

static size_t
scorpi_device_count(const struct scorpi_device *devices)
{
	size_t count;

	count = 0;
	for (; devices != NULL; devices = devices->next)
		count++;
	return (count);
}

static void
scorpi_normalized_prop_reset(struct scorpi_normalized_prop *prop)
{
	if (prop == NULL)
		return;
	free(prop->name);
	if (prop->kind == SCORPI_PROP_STRING)
		free(prop->value.string);
	memset(prop, 0, sizeof(*prop));
}

static void
scorpi_normalized_device_reset(struct scorpi_normalized_device *device)
{
	size_t i;

	if (device == NULL)
		return;
	free(device->device);
	free(device->id);
	free(device->parent);
	for (i = 0; i < device->prop_count; i++)
		scorpi_normalized_prop_reset(&device->props[i]);
	free(device->props);
	memset(device, 0, sizeof(*device));
}

static int
scorpi_normalized_prop_compare(const struct scorpi_normalized_prop *lhs,
    const struct scorpi_normalized_prop *rhs)
{
	int cmp;

	cmp = strcmp(lhs->name, rhs->name);
	if (cmp != 0)
		return (cmp);
	if (lhs->kind != rhs->kind)
		return (lhs->kind < rhs->kind ? -1 : 1);

	switch (lhs->kind) {
	case SCORPI_PROP_STRING:
		return (scorpi_string_compare_nullable(lhs->value.string,
		    rhs->value.string));
	case SCORPI_PROP_BOOL:
		if (lhs->value.boolean == rhs->value.boolean)
			return (0);
		return (lhs->value.boolean ? 1 : -1);
	case SCORPI_PROP_U64:
		if (lhs->value.u64 == rhs->value.u64)
			return (0);
		return (lhs->value.u64 < rhs->value.u64 ? -1 : 1);
	}

	return (0);
}

static int
scorpi_normalized_prop_compare_qsort(const void *lhs, const void *rhs)
{
	return (scorpi_normalized_prop_compare(lhs, rhs));
}

static scorpi_error_t
scorpi_normalized_prop_copy(const struct scorpi_prop *src,
    struct scorpi_normalized_prop *dst)
{
	memset(dst, 0, sizeof(*dst));

	dst->name = strdup(src->name);
	if (dst->name == NULL)
		return (SCORPI_ERR_RUNTIME);

	dst->kind = src->kind;
	switch (src->kind) {
	case SCORPI_PROP_STRING:
		if (src->value.string != NULL) {
			dst->value.string = strdup(src->value.string);
			if (dst->value.string == NULL) {
				scorpi_normalized_prop_reset(dst);
				return (SCORPI_ERR_RUNTIME);
			}
		}
		break;
	case SCORPI_PROP_BOOL:
		dst->value.boolean = src->value.boolean;
		break;
	case SCORPI_PROP_U64:
		dst->value.u64 = src->value.u64;
		break;
	}

	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_normalized_props_from_list(const struct scorpi_prop *props,
    size_t *out_count, struct scorpi_normalized_prop **out_props)
{
	struct scorpi_normalized_prop *normalized_props;
	const struct scorpi_prop *prop;
	size_t count, i;
	scorpi_error_t error;

	*out_count = 0;
	*out_props = NULL;

	count = scorpi_prop_count(props);
	if (count == 0)
		return (SCORPI_OK);

	normalized_props = calloc(count, sizeof(*normalized_props));
	if (normalized_props == NULL)
		return (SCORPI_ERR_RUNTIME);

	i = 0;
	for (prop = props; prop != NULL; prop = prop->next) {
		error = scorpi_normalized_prop_copy(prop, &normalized_props[i]);
		if (error != SCORPI_OK) {
			while (i > 0) {
				i--;
				scorpi_normalized_prop_reset(&normalized_props[i]);
			}
			free(normalized_props);
			return (error);
		}
		i++;
	}

	qsort(normalized_props, count, sizeof(*normalized_props),
	    scorpi_normalized_prop_compare_qsort);
	*out_count = count;
	*out_props = normalized_props;
	return (SCORPI_OK);
}

static int
scorpi_normalized_device_compare(const struct scorpi_normalized_device *lhs,
    const struct scorpi_normalized_device *rhs)
{
	size_t i, count;
	int cmp;

	cmp = scorpi_string_compare_nullable(lhs->id, rhs->id);
	if (cmp != 0)
		return (cmp);
	if (lhs->bus != rhs->bus)
		return (lhs->bus < rhs->bus ? -1 : 1);
	cmp = scorpi_string_compare_nullable(lhs->device, rhs->device);
	if (cmp != 0)
		return (cmp);
	if (lhs->slot != rhs->slot)
		return (lhs->slot < rhs->slot ? -1 : 1);
	cmp = scorpi_string_compare_nullable(lhs->parent, rhs->parent);
	if (cmp != 0)
		return (cmp);
	if (lhs->prop_count != rhs->prop_count)
		return (lhs->prop_count < rhs->prop_count ? -1 : 1);

	count = lhs->prop_count;
	for (i = 0; i < count; i++) {
		cmp = scorpi_normalized_prop_compare(&lhs->props[i],
		    &rhs->props[i]);
		if (cmp != 0)
			return (cmp);
	}

	return (0);
}

static int
scorpi_normalized_device_compare_qsort(const void *lhs, const void *rhs)
{
	return (scorpi_normalized_device_compare(lhs, rhs));
}

static scorpi_error_t
scorpi_normalized_device_copy(const struct scorpi_device *src,
    struct scorpi_normalized_device *dst)
{
	const struct scorpi_prop *parent_prop;
	const char *id;
	scorpi_error_t error;

	memset(dst, 0, sizeof(*dst));

	dst->device = strdup(src->device);
	if (dst->device == NULL)
		return (SCORPI_ERR_RUNTIME);
	dst->bus = src->bus;
	dst->slot = src->slot;

	id = scorpi_device_get_id(src);
	if (id != NULL) {
		dst->id = strdup(id);
		if (dst->id == NULL) {
			scorpi_normalized_device_reset(dst);
			return (SCORPI_ERR_RUNTIME);
		}
	}

	parent_prop = scorpi_device_find_prop(src, "parent");
	if (parent_prop != NULL && parent_prop->kind == SCORPI_PROP_STRING &&
	    parent_prop->value.string != NULL) {
		dst->parent = strdup(parent_prop->value.string);
		if (dst->parent == NULL) {
			scorpi_normalized_device_reset(dst);
			return (SCORPI_ERR_RUNTIME);
		}
	}

	error = scorpi_normalized_props_from_list(src->props, &dst->prop_count,
	    &dst->props);
	if (error != SCORPI_OK) {
		scorpi_normalized_device_reset(dst);
		return (error);
	}

	return (SCORPI_OK);
}

scorpi_error_t
scorpi_vm_normalize(const struct scorpi_vm *vm,
    struct scorpi_normalized_vm **out_vm)
{
	struct scorpi_normalized_vm *normalized_vm;
	const struct scorpi_device *device;
	size_t device_count, i;
	scorpi_error_t error;

	if (vm == NULL || out_vm == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	*out_vm = NULL;

	normalized_vm = calloc(1, sizeof(*normalized_vm));
	if (normalized_vm == NULL)
		return (SCORPI_ERR_RUNTIME);

	error = scorpi_normalized_props_from_list(vm->props,
	    &normalized_vm->prop_count, &normalized_vm->props);
	if (error != SCORPI_OK) {
		scorpi_normalized_vm_destroy(normalized_vm);
		return (error);
	}

	device_count = scorpi_device_count(vm->devices);
	if (device_count != 0) {
		normalized_vm->devices = calloc(device_count,
		    sizeof(*normalized_vm->devices));
		if (normalized_vm->devices == NULL) {
			scorpi_normalized_vm_destroy(normalized_vm);
			return (SCORPI_ERR_RUNTIME);
		}
		normalized_vm->device_count = device_count;
		i = 0;
		for (device = vm->devices; device != NULL; device = device->next) {
			error = scorpi_normalized_device_copy(device,
			    &normalized_vm->devices[i]);
			if (error != SCORPI_OK) {
				scorpi_normalized_vm_destroy(normalized_vm);
				return (error);
			}
			i++;
		}
		qsort(normalized_vm->devices, normalized_vm->device_count,
		    sizeof(*normalized_vm->devices),
		    scorpi_normalized_device_compare_qsort);
	}

	*out_vm = normalized_vm;
	return (SCORPI_OK);
}

void
scorpi_normalized_vm_destroy(struct scorpi_normalized_vm *vm)
{
	size_t i;

	if (vm == NULL)
		return;
	for (i = 0; i < vm->prop_count; i++)
		scorpi_normalized_prop_reset(&vm->props[i]);
	free(vm->props);
	for (i = 0; i < vm->device_count; i++)
		scorpi_normalized_device_reset(&vm->devices[i]);
	free(vm->devices);
	free(vm);
}

bool
scorpi_normalized_vm_equal(const struct scorpi_normalized_vm *lhs,
    const struct scorpi_normalized_vm *rhs)
{
	size_t i;

	if (lhs == NULL || rhs == NULL)
		return (lhs == rhs);
	if (lhs->prop_count != rhs->prop_count ||
	    lhs->device_count != rhs->device_count)
		return (false);

	for (i = 0; i < lhs->prop_count; i++) {
		if (scorpi_normalized_prop_compare(&lhs->props[i],
		    &rhs->props[i]) != 0)
			return (false);
	}
	for (i = 0; i < lhs->device_count; i++) {
		if (scorpi_normalized_device_compare(&lhs->devices[i],
		    &rhs->devices[i]) != 0)
			return (false);
	}
	return (true);
}

const struct scorpi_normalized_prop *
scorpi_normalized_vm_find_prop(const struct scorpi_normalized_vm *vm,
    const char *name)
{
	size_t i;

	if (vm == NULL || name == NULL)
		return (NULL);
	for (i = 0; i < vm->prop_count; i++) {
		if (strcmp(vm->props[i].name, name) == 0)
			return (&vm->props[i]);
	}
	return (NULL);
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

static bool
scorpi_yaml_scalar_equals(const yaml_node_t *node, const char *value)
{
	size_t length;

	if (node == NULL || value == NULL || node->type != YAML_SCALAR_NODE)
		return (false);
	length = strlen(value);
	return (node->data.scalar.length == length &&
	    memcmp(node->data.scalar.value, value, length) == 0);
}

static yaml_node_t *
scorpi_yaml_mapping_lookup(yaml_document_t *document, yaml_node_t *mapping,
    const char *key)
{
	yaml_node_pair_t *pair;
	yaml_node_t *key_node;

	if (document == NULL || mapping == NULL || key == NULL ||
	    mapping->type != YAML_MAPPING_NODE)
		return (NULL);

	for (pair = mapping->data.mapping.pairs.start;
	    pair < mapping->data.mapping.pairs.top; pair++) {
		key_node = yaml_document_get_node(document, pair->key);
		if (scorpi_yaml_scalar_equals(key_node, key))
			return (yaml_document_get_node(document, pair->value));
	}

	return (NULL);
}

static scorpi_error_t
scorpi_yaml_parse_u64(const yaml_node_t *node, uint64_t *value)
{
	char *endptr;
	unsigned long long parsed;

	if (node == NULL || value == NULL || node->type != YAML_SCALAR_NODE)
		return (SCORPI_ERR_VALIDATION);

	errno = 0;
	parsed = strtoull((const char *)node->data.scalar.value, &endptr, 10);
	if (errno != 0 || endptr == (char *)node->data.scalar.value ||
	    *endptr != '\0')
		return (SCORPI_ERR_VALIDATION);

	*value = (uint64_t)parsed;
	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_yaml_parse_memory(const yaml_node_t *node, uint64_t *value)
{
	if (node == NULL || value == NULL || node->type != YAML_SCALAR_NODE)
		return (SCORPI_ERR_VALIDATION);
	if (expand_number((const char *)node->data.scalar.value, value) != 0)
		return (SCORPI_ERR_VALIDATION);
	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_yaml_set_vm_scalar_prop(scorpi_vm_t vm, const char *name,
    const yaml_node_t *node)
{
	uint64_t value_u64;

	if (scorpi_yaml_scalar_equals(node, "true"))
		return (scorpi_vm_set_prop_bool(vm, name, true));
	if (scorpi_yaml_scalar_equals(node, "false"))
		return (scorpi_vm_set_prop_bool(vm, name, false));
	if (scorpi_yaml_parse_u64(node, &value_u64) == SCORPI_OK)
		return (scorpi_vm_set_prop_u64(vm, name, value_u64));
	return (scorpi_vm_set_prop_string(vm, name,
	    (const char *)node->data.scalar.value));
}

static scorpi_error_t
scorpi_yaml_set_device_scalar_prop(scorpi_device_t dev, const char *name,
    const yaml_node_t *node)
{
	uint64_t value_u64;

	if (scorpi_yaml_scalar_equals(node, "true"))
		return (scorpi_device_set_prop_bool(dev, name, true));
	if (scorpi_yaml_scalar_equals(node, "false"))
		return (scorpi_device_set_prop_bool(dev, name, false));
	if (scorpi_yaml_parse_u64(node, &value_u64) == SCORPI_OK)
		return (scorpi_device_set_prop_u64(dev, name, value_u64));
	return (scorpi_device_set_prop_string(dev, name,
	    (const char *)node->data.scalar.value));
}

static scorpi_error_t
scorpi_yaml_build_device(yaml_document_t *document, const char *bus_name,
    yaml_node_t *device_node, scorpi_device_t *out_dev)
{
	yaml_node_pair_t *pair;
	yaml_node_t *device_name_node, *key_node, *slot_node, *value_node;
	scorpi_device_t dev;
	uint64_t slot;
	scorpi_error_t error;

	if (document == NULL || bus_name == NULL || device_node == NULL ||
	    out_dev == NULL || device_node->type != YAML_MAPPING_NODE)
		return (SCORPI_ERR_VALIDATION);
	*out_dev = NULL;

	device_name_node = scorpi_yaml_mapping_lookup(document, device_node,
	    "device");
	if (device_name_node == NULL || device_name_node->type != YAML_SCALAR_NODE)
		return (SCORPI_ERR_VALIDATION);

	slot = SCORPI_PCI_SLOT_AUTO;
	if (strcmp(bus_name, "pci") == 0) {
		slot_node = scorpi_yaml_mapping_lookup(document, device_node,
		    "slot");
		if (slot_node != NULL) {
			error = scorpi_yaml_parse_u64(slot_node, &slot);
			if (error != SCORPI_OK)
				return (error);
		}
		error = scorpi_create_pci_device(
		    (const char *)device_name_node->data.scalar.value, slot,
		    &dev);
	} else if (strcmp(bus_name, "usb") == 0) {
		error = scorpi_create_usb_device(
		    (const char *)device_name_node->data.scalar.value, &dev);
	} else if (strcmp(bus_name, "lpc") == 0) {
		error = scorpi_create_lpc_device(
		    (const char *)device_name_node->data.scalar.value, &dev);
	} else {
		return (SCORPI_ERR_UNSUPPORTED_DEVICE);
	}
	if (error != SCORPI_OK)
		return (error);

	for (pair = device_node->data.mapping.pairs.start;
	    pair < device_node->data.mapping.pairs.top; pair++) {
		key_node = yaml_document_get_node(document, pair->key);
		value_node = yaml_document_get_node(document, pair->value);
		if (key_node == NULL || key_node->type != YAML_SCALAR_NODE ||
		    value_node == NULL)
			continue;
		if (scorpi_yaml_scalar_equals(key_node, "device") ||
		    scorpi_yaml_scalar_equals(key_node, "slot"))
			continue;
		if (value_node->type != YAML_SCALAR_NODE) {
			scorpi_destroy_device(dev);
			return (SCORPI_ERR_VALIDATION);
		}
		error = scorpi_yaml_set_device_scalar_prop(dev,
		    (const char *)key_node->data.scalar.value, value_node);
		if (error != SCORPI_OK) {
			scorpi_destroy_device(dev);
			return (error);
		}
	}

	*out_dev = dev;
	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_yaml_load_devices(scorpi_vm_t vm, yaml_document_t *document,
    yaml_node_t *devices_node)
{
	yaml_node_item_t *item;
	yaml_node_pair_t *pair;
	yaml_node_t *bus_key_node, *bus_value_node, *device_node;
	scorpi_device_t dev;
	scorpi_error_t error;

	if (devices_node == NULL)
		return (SCORPI_OK);
	if (devices_node->type != YAML_MAPPING_NODE)
		return (SCORPI_ERR_VALIDATION);

	for (pair = devices_node->data.mapping.pairs.start;
	    pair < devices_node->data.mapping.pairs.top; pair++) {
		bus_key_node = yaml_document_get_node(document, pair->key);
		bus_value_node = yaml_document_get_node(document, pair->value);
		if (bus_key_node == NULL || bus_key_node->type != YAML_SCALAR_NODE ||
		    bus_value_node == NULL)
			return (SCORPI_ERR_VALIDATION);
		if (bus_value_node->type != YAML_SEQUENCE_NODE)
			return (SCORPI_ERR_VALIDATION);

		for (item = bus_value_node->data.sequence.items.start;
		    item < bus_value_node->data.sequence.items.top; item++) {
			device_node = yaml_document_get_node(document, *item);
			error = scorpi_yaml_build_device(document,
			    (const char *)bus_key_node->data.scalar.value,
			    device_node, &dev);
			if (error != SCORPI_OK)
				return (error);
			error = scorpi_vm_add_device(vm, dev);
			if (error != SCORPI_OK) {
				scorpi_destroy_device(dev);
				return (error);
			}
		}
	}

	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_yaml_load_root_mapping(scorpi_vm_t vm, yaml_document_t *document,
    yaml_node_t *root)
{
	yaml_node_pair_t *pair;
	yaml_node_t *key_node, *value_node, *size_node;
	uint64_t value_u64;
	scorpi_error_t error;

	for (pair = root->data.mapping.pairs.start;
	    pair < root->data.mapping.pairs.top; pair++) {
		key_node = yaml_document_get_node(document, pair->key);
		value_node = yaml_document_get_node(document, pair->value);
		if (key_node == NULL || key_node->type != YAML_SCALAR_NODE ||
		    value_node == NULL)
			return (SCORPI_ERR_VALIDATION);

		if (scorpi_yaml_scalar_equals(key_node, "cpu")) {
			if (value_node->type == YAML_SCALAR_NODE) {
				error = scorpi_yaml_parse_u64(value_node, &value_u64);
				if (error != SCORPI_OK)
					return (error);
				error = scorpi_vm_set_cpu(vm, value_u64);
			} else if (value_node->type == YAML_MAPPING_NODE) {
				size_node = scorpi_yaml_mapping_lookup(document,
				    value_node, "cores");
				error = scorpi_yaml_parse_u64(size_node, &value_u64);
				if (error != SCORPI_OK)
					return (error);
				error = scorpi_vm_set_cpu(vm, value_u64);
			} else {
				return (SCORPI_ERR_VALIDATION);
			}
		} else if (scorpi_yaml_scalar_equals(key_node, "memory")) {
			if (value_node->type == YAML_SCALAR_NODE) {
				error = scorpi_yaml_parse_memory(value_node, &value_u64);
				if (error != SCORPI_OK)
					return (error);
				error = scorpi_vm_set_ram(vm, value_u64);
			} else if (value_node->type == YAML_MAPPING_NODE) {
				size_node = scorpi_yaml_mapping_lookup(document,
				    value_node, "size");
				error = scorpi_yaml_parse_memory(size_node, &value_u64);
				if (error != SCORPI_OK)
					return (error);
				error = scorpi_vm_set_ram(vm, value_u64);
			} else {
				return (SCORPI_ERR_VALIDATION);
			}
		} else if (scorpi_yaml_scalar_equals(key_node, "devices")) {
			error = scorpi_yaml_load_devices(vm, document, value_node);
		} else if (value_node->type == YAML_SCALAR_NODE) {
			error = scorpi_yaml_set_vm_scalar_prop(vm,
			    (const char *)key_node->data.scalar.value, value_node);
		} else {
			return (SCORPI_ERR_VALIDATION);
		}

		if (error != SCORPI_OK)
			return (error);
	}

	return (SCORPI_OK);
}

scorpi_error_t
scorpi_load_vm_from_yaml(const char *yaml, scorpi_vm_t *out_vm)
{
	yaml_document_t document;
	yaml_node_t *root;
	yaml_parser_t parser;
	scorpi_vm_t vm;
	scorpi_error_t error;
	int parser_initialized;
	int document_loaded;

	if (yaml == NULL || out_vm == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	*out_vm = NULL;

	parser_initialized = 0;
	document_loaded = 0;

	if (!yaml_parser_initialize(&parser))
		return (SCORPI_ERR_RUNTIME);
	parser_initialized = 1;
	yaml_parser_set_input_string(&parser, (const unsigned char *)yaml,
	    strlen(yaml));

	if (!yaml_parser_load(&parser, &document)) {
		yaml_parser_delete(&parser);
		return (SCORPI_ERR_YAML_PARSE);
	}
	document_loaded = 1;

	error = scorpi_create_vm(&vm);
	if (error != SCORPI_OK)
		goto out;

	root = yaml_document_get_root_node(&document);
	if (root != NULL) {
		if (root->type != YAML_MAPPING_NODE) {
			error = SCORPI_ERR_VALIDATION;
			goto out_destroy_vm;
		}
		error = scorpi_yaml_load_root_mapping(vm, &document, root);
		if (error != SCORPI_OK)
			goto out_destroy_vm;
	}

	*out_vm = vm;
	error = SCORPI_OK;
	goto out;

out_destroy_vm:
	scorpi_destroy_vm(vm);
out:
	if (document_loaded)
		yaml_document_delete(&document);
	if (parser_initialized)
		yaml_parser_delete(&parser);
	return (error);
}
