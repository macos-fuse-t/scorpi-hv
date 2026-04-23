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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <sysexits.h>
#include <sys/wait.h>
#include <unistd.h>
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

static const struct scorpi_normalized_prop *
scorpi_normalized_device_find_prop(const struct scorpi_normalized_device *device,
    const char *name)
{
	size_t i;

	if (device == NULL || name == NULL)
		return (NULL);
	for (i = 0; i < device->prop_count; i++) {
		if (strcmp(device->props[i].name, name) == 0)
			return (&device->props[i]);
	}
	return (NULL);
}

static scorpi_error_t
scorpi_normalized_prop_to_string(const struct scorpi_normalized_prop *prop,
    char **out_value)
{
	char buf[32];
	int len;

	if (prop == NULL || out_value == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	*out_value = NULL;

	switch (prop->kind) {
	case SCORPI_PROP_STRING:
		if (prop->value.string == NULL)
			return (SCORPI_ERR_VALIDATION);
		*out_value = strdup(prop->value.string);
		break;
	case SCORPI_PROP_BOOL:
		*out_value = strdup(prop->value.boolean ? "true" : "false");
		break;
	case SCORPI_PROP_U64:
		len = snprintf(buf, sizeof(buf), "%llu",
		    (unsigned long long)prop->value.u64);
		if (len < 0 || (size_t)len >= sizeof(buf))
			return (SCORPI_ERR_RUNTIME);
		*out_value = strdup(buf);
		break;
	}

	if (*out_value == NULL)
		return (SCORPI_ERR_RUNTIME);
	return (SCORPI_OK);
}

static nvlist_t *
scorpi_config_get_or_create_node(nvlist_t *root, const char *path)
{
	char *copy, *name, *tofree;
	nvlist_t *child;

	if (root == NULL || path == NULL || *path == '\0')
		return (root);

	copy = strdup(path);
	if (copy == NULL)
		return (NULL);
	tofree = copy;

	while ((name = strsep(&copy, ".")) != NULL) {
		if (*name == '\0') {
			free(tofree);
			return (NULL);
		}
		if (nvlist_exists_nvlist(root, name)) {
			root = __DECONST(nvlist_t *, nvlist_get_nvlist(root, name));
			continue;
		}
		if (nvlist_exists(root, name)) {
			free(tofree);
			return (NULL);
		}
		child = nvlist_create(0);
		if (child == NULL) {
			free(tofree);
			return (NULL);
		}
		nvlist_move_nvlist(root, name, child);
		root = child;
	}

	free(tofree);
	return (root);
}

const nvlist_t *
scorpi_config_find_node(const nvlist_t *config, const char *path)
{
	char *copy, *name, *tofree;

	if (config == NULL || path == NULL || *path == '\0')
		return (config);

	copy = strdup(path);
	if (copy == NULL)
		return (NULL);
	tofree = copy;

	while ((name = strsep(&copy, ".")) != NULL) {
		if (*name == '\0' || !nvlist_exists_nvlist(config, name)) {
			free(tofree);
			return (NULL);
		}
		config = nvlist_get_nvlist(config, name);
	}

	free(tofree);
	return (config);
}

const char *
scorpi_config_get_value(const nvlist_t *config, const char *path)
{
	const char *name;
	char *copy, *node_path;
	const nvlist_t *node;

	if (config == NULL || path == NULL || *path == '\0')
		return (NULL);

	name = strrchr(path, '.');
	if (name == NULL) {
		if (!nvlist_exists_string(config, path))
			return (NULL);
		return (nvlist_get_string(config, path));
	}

	copy = strdup(path);
	if (copy == NULL)
		return (NULL);
	copy[name - path] = '\0';
	node_path = copy;
	name++;
	node = scorpi_config_find_node(config, node_path);
	free(copy);
	if (node == NULL || !nvlist_exists_string(node, name))
		return (NULL);
	return (nvlist_get_string(node, name));
}

static scorpi_error_t
scorpi_config_set_value(nvlist_t *root, const char *path, const char *value)
{
	char *copy, *leaf;
	nvlist_t *node;

	if (root == NULL || path == NULL || *path == '\0' || value == NULL)
		return (SCORPI_ERR_INVALID_ARG);

	copy = strdup(path);
	if (copy == NULL)
		return (SCORPI_ERR_RUNTIME);

	leaf = strrchr(copy, '.');
	if (leaf == NULL) {
		node = root;
		leaf = copy;
	} else {
		*leaf = '\0';
		leaf++;
		node = scorpi_config_get_or_create_node(root, copy);
		if (node == NULL) {
			free(copy);
			return (SCORPI_ERR_RUNTIME);
		}
	}

	if (nvlist_exists_string(node, leaf))
		nvlist_free_string(node, leaf);
	else if (nvlist_exists(node, leaf)) {
		free(copy);
		return (SCORPI_ERR_RUNTIME);
	}
	nvlist_add_string(node, leaf, value);
	free(copy);
	return (SCORPI_OK);
}

static bool
scorpi_config_value_equal(const nvlist_t *lhs, const nvlist_t *rhs,
    const char *name, int type)
{
	switch (type) {
	case NV_TYPE_STRING:
		return (strcmp(nvlist_get_string(lhs, name),
		    nvlist_get_string(rhs, name)) == 0);
	case NV_TYPE_NVLIST:
		return (scorpi_config_equal(nvlist_get_nvlist(lhs, name),
		    nvlist_get_nvlist(rhs, name)));
	default:
		return (false);
	}
}

bool
scorpi_config_equal(const nvlist_t *lhs, const nvlist_t *rhs)
{
	void *cookie;
	const char *name;
	int type;

	if (lhs == NULL || rhs == NULL)
		return (lhs == rhs);

	cookie = NULL;
	while ((name = nvlist_next(lhs, &type, &cookie)) != NULL) {
		if (!nvlist_exists_type(rhs, name, type) ||
		    !scorpi_config_value_equal(lhs, rhs, name, type))
			return (false);
	}

	cookie = NULL;
	while ((name = nvlist_next(rhs, &type, &cookie)) != NULL) {
		if (!nvlist_exists_type(lhs, name, type) ||
		    !scorpi_config_value_equal(rhs, lhs, name, type))
			return (false);
	}

	return (true);
}

void
scorpi_config_destroy(nvlist_t *config)
{
	nvlist_destroy(config);
}

struct scorpi_imported_xhci {
	scorpi_device_t dev;
	uint64_t usb_bus;
	unsigned int pci_bus;
	unsigned int slot;
	unsigned int func;
};

static bool
scorpi_parse_bool_string(const char *value, bool *out_value)
{
	if (value == NULL || out_value == NULL)
		return (false);
	if (strcmp(value, "true") == 0) {
		*out_value = true;
		return (true);
	}
	if (strcmp(value, "false") == 0) {
		*out_value = false;
		return (true);
	}
	return (false);
}

static bool
scorpi_parse_u64_string(const char *value, uint64_t *out_value)
{
	char *endptr;
	unsigned long long parsed;

	if (value == NULL || out_value == NULL)
		return (false);

	errno = 0;
	parsed = strtoull(value, &endptr, 10);
	if (errno != 0 || endptr == value || *endptr != '\0')
		return (false);

	*out_value = (uint64_t)parsed;
	return (true);
}

static scorpi_error_t
scorpi_vm_set_inferred_prop(scorpi_vm_t vm, const char *name, const char *value)
{
	bool bool_value;
	uint64_t u64_value;

	if (scorpi_parse_bool_string(value, &bool_value))
		return (scorpi_vm_set_prop_bool(vm, name, bool_value));
	if (scorpi_parse_u64_string(value, &u64_value))
		return (scorpi_vm_set_prop_u64(vm, name, u64_value));
	return (scorpi_vm_set_prop_string(vm, name, value));
}

static scorpi_error_t
scorpi_device_set_inferred_prop(scorpi_device_t dev, const char *name,
    const char *value)
{
	bool bool_value;
	uint64_t u64_value;

	if (scorpi_parse_bool_string(value, &bool_value))
		return (scorpi_device_set_prop_bool(dev, name, bool_value));
	if (scorpi_parse_u64_string(value, &u64_value))
		return (scorpi_device_set_prop_u64(dev, name, u64_value));
	return (scorpi_device_set_prop_string(dev, name, value));
}

static bool
scorpi_prop_name_matches(const char *name, const char *skip_name)
{
	return (skip_name != NULL && strcmp(name, skip_name) == 0);
}

static scorpi_error_t
scorpi_import_device_props(const nvlist_t *config, scorpi_device_t dev,
    const char *skip_name1, const char *skip_name2)
{
	void *cookie;
	const char *name;
	int type;
	scorpi_error_t error;

	cookie = NULL;
	while ((name = nvlist_next(config, &type, &cookie)) != NULL) {
		if (scorpi_prop_name_matches(name, skip_name1) ||
		    scorpi_prop_name_matches(name, skip_name2))
			continue;
		if (type != NV_TYPE_STRING)
			return (SCORPI_ERR_UNSUPPORTED);
		error = scorpi_device_set_inferred_prop(dev, name,
		    nvlist_get_string(config, name));
		if (error != SCORPI_OK)
			return (error);
	}
	return (SCORPI_OK);
}

static bool
scorpi_legacy_ahci_device_type(const char *device_name, const char **type)
{
	if (device_name == NULL || type == NULL)
		return (false);
	if (strcmp(device_name, "ahci-hd") == 0) {
		*type = "hd";
		return (true);
	}
	if (strcmp(device_name, "ahci-cd") == 0) {
		*type = "cd";
		return (true);
	}
	return (false);
}

static scorpi_error_t
scorpi_import_ahci_device(const nvlist_t *func_node, uint64_t slot,
    scorpi_vm_t vm)
{
	void *cookie;
	const char *controller_name, *port_name, *prop_name;
	const nvlist_t *port_node, *ports_node;
	scorpi_device_t dev;
	int controller_type, port_type, prop_type;
	void *prop_cookie;
	char prop_path[128];
	scorpi_error_t error;

	ports_node = scorpi_config_find_node(func_node, "port");
	if (ports_node == NULL)
		return (SCORPI_ERR_UNSUPPORTED);

	error = scorpi_create_pci_device("ahci", slot, &dev);
	if (error != SCORPI_OK)
		return (error);

	cookie = NULL;
	while ((controller_name = nvlist_next(func_node, &controller_type,
	    &cookie)) != NULL) {
		if (scorpi_prop_name_matches(controller_name, "device") ||
		    scorpi_prop_name_matches(controller_name, "port"))
			continue;
		if (controller_type != NV_TYPE_STRING) {
			scorpi_destroy_device(dev);
			return (SCORPI_ERR_UNSUPPORTED);
		}
		error = scorpi_device_set_inferred_prop(dev, controller_name,
		    nvlist_get_string(func_node, controller_name));
		if (error != SCORPI_OK) {
			scorpi_destroy_device(dev);
			return (error);
		}
	}

	cookie = NULL;
	while ((port_name = nvlist_next(ports_node, &port_type,
	    &cookie)) != NULL) {
		if (port_type != NV_TYPE_NVLIST) {
			scorpi_destroy_device(dev);
			return (SCORPI_ERR_UNSUPPORTED);
		}

		port_node = nvlist_get_nvlist(ports_node, port_name);
		prop_cookie = NULL;
		while ((prop_name = nvlist_next(port_node, &prop_type,
		    &prop_cookie)) != NULL) {
			if (prop_type != NV_TYPE_STRING) {
				scorpi_destroy_device(dev);
				return (SCORPI_ERR_UNSUPPORTED);
			}
			if (snprintf(prop_path, sizeof(prop_path), "port.%s.%s",
			    port_name, prop_name) >= (int)sizeof(prop_path)) {
				scorpi_destroy_device(dev);
				return (SCORPI_ERR_RUNTIME);
			}
			error = scorpi_device_set_inferred_prop(dev, prop_path,
			    nvlist_get_string(port_node, prop_name));
			if (error != SCORPI_OK) {
				scorpi_destroy_device(dev);
				return (error);
			}
		}
	}

	error = scorpi_vm_add_device(vm, dev);
	if (error != SCORPI_OK) {
		scorpi_destroy_device(dev);
		return (error);
	}

	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_import_compatible_ahci_device(const nvlist_t *func_node, uint64_t slot,
    scorpi_vm_t vm)
{
	return (scorpi_import_ahci_device(func_node, slot, vm));
}

static scorpi_error_t
scorpi_assign_imported_xhci_id(struct scorpi_imported_xhci *xhci)
{
	char id[64];
	const struct scorpi_prop *prop;

	if (xhci == NULL || xhci->dev == NULL)
		return (SCORPI_ERR_INVALID_ARG);

	prop = scorpi_device_find_prop(xhci->dev, "id");
	if (prop != NULL)
		return (SCORPI_OK);

	snprintf(id, sizeof(id), "__xhci_%u_%u_%u", xhci->pci_bus, xhci->slot,
	    xhci->func);
	return (scorpi_device_set_prop_string(xhci->dev, "id", id));
}

static const char *
scorpi_imported_xhci_id(const struct scorpi_imported_xhci *xhci)
{
	const struct scorpi_prop *prop;

	if (xhci == NULL || xhci->dev == NULL)
		return (NULL);

	prop = scorpi_device_find_prop(xhci->dev, "id");
	if (prop == NULL || prop->kind != SCORPI_PROP_STRING)
		return (NULL);
	return (prop->value.string);
}

static scorpi_error_t
scorpi_add_imported_xhci(struct scorpi_imported_xhci **xhcis, size_t *count,
    const struct scorpi_imported_xhci *xhci)
{
	struct scorpi_imported_xhci *new_xhcis;

	new_xhcis = realloc(*xhcis, (*count + 1) * sizeof(*new_xhcis));
	if (new_xhcis == NULL)
		return (SCORPI_ERR_RUNTIME);

	new_xhcis[*count] = *xhci;
	*xhcis = new_xhcis;
	(*count)++;
	return (SCORPI_OK);
}

static struct scorpi_imported_xhci *
scorpi_find_imported_xhci_by_usb_bus(struct scorpi_imported_xhci *xhcis,
	size_t count, uint64_t usb_bus)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (xhcis[i].usb_bus == usb_bus)
			return (&xhcis[i]);
	}
	return (NULL);
}

static scorpi_error_t
scorpi_parse_config_memory(const char *value, uint64_t *memory_size)
{
	if (value == NULL || memory_size == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	if (expand_number(value, memory_size) != 0)
		return (SCORPI_ERR_VALIDATION);
	return (SCORPI_OK);
}

static bool
scorpi_skip_imported_vm_scalar(const char *name)
{
	static const char *const skipped[] = {
		"name",
		"uuid",
		"cpus",
		"cores",
		"sockets",
		"threads",
		"comm_sock",
	};
	size_t i;

	if (name == NULL)
		return (true);

	for (i = 0; i < sizeof(skipped) / sizeof(skipped[0]); i++) {
		if (strcmp(name, skipped[i]) == 0)
			return (true);
	}

	return (false);
}

static scorpi_error_t
scorpi_import_generic_vm_scalars(const nvlist_t *config, scorpi_vm_t vm)
{
	void *cookie;
	const char *name;
	int type;
	scorpi_error_t error;

	cookie = NULL;
	while ((name = nvlist_next(config, &type, &cookie)) != NULL) {
		if (type != NV_TYPE_STRING || scorpi_skip_imported_vm_scalar(name))
			continue;

		error = scorpi_vm_set_inferred_prop(vm, name,
		    nvlist_get_string(config, name));
		if (error != SCORPI_OK)
			return (error);
	}

	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_import_vm_scalars(const nvlist_t *config, scorpi_vm_t vm)
{
	const nvlist_t *gic, *memory;
	const char *memory_size, *name, *uuid, *value;
	uint64_t cpu_count, ram_bytes;
	scorpi_error_t error;

	name = scorpi_config_get_value(config, "name");
	if (name != NULL) {
		error = scorpi_vm_set_prop_string(vm, "name", name);
		if (error != SCORPI_OK)
			return (error);
	}

	uuid = scorpi_config_get_value(config, "uuid");
	if (uuid != NULL) {
		error = scorpi_vm_set_prop_string(vm, "uuid", uuid);
		if (error != SCORPI_OK)
			return (error);
	}

	value = scorpi_config_get_value(config, "threads");
	if (value != NULL && strcmp(value, "1") != 0)
		return (SCORPI_ERR_UNSUPPORTED);
	value = scorpi_config_get_value(config, "sockets");
	if (value != NULL && strcmp(value, "1") != 0)
		return (SCORPI_ERR_UNSUPPORTED);

	value = scorpi_config_get_value(config, "cpus");
	if (value != NULL) {
		if (!scorpi_parse_u64_string(value, &cpu_count) || cpu_count == 0)
			return (SCORPI_ERR_VALIDATION);
		error = scorpi_vm_set_cpu(vm, cpu_count);
		if (error != SCORPI_OK)
			return (error);
	} else {
		value = scorpi_config_get_value(config, "cores");
		if (value != NULL) {
			if (!scorpi_parse_u64_string(value, &cpu_count) ||
			    cpu_count == 0)
				return (SCORPI_ERR_VALIDATION);
			error = scorpi_vm_set_cpu(vm, cpu_count);
			if (error != SCORPI_OK)
				return (error);
		} else {
			error = scorpi_vm_set_cpu(vm, 1);
			if (error != SCORPI_OK)
				return (error);
		}
	}

	value = scorpi_config_get_value(config, "cores");
	if (value != NULL && scorpi_parse_u64_string(value, &cpu_count)) {
		const struct scorpi_prop *cpu_prop;

		cpu_prop = scorpi_vm_find_prop(vm, "cpu.cores");
		if (cpu_prop == NULL || cpu_prop->kind != SCORPI_PROP_U64 ||
		    cpu_prop->value.u64 != cpu_count)
			return (SCORPI_ERR_UNSUPPORTED);
	}

	memory_size = scorpi_config_get_value(config, "memory.size");
	if (memory_size == NULL)
		return (SCORPI_ERR_VALIDATION);
	error = scorpi_parse_config_memory(memory_size, &ram_bytes);
	if (error != SCORPI_OK)
		return (error);
	error = scorpi_vm_set_ram(vm, ram_bytes);
	if (error != SCORPI_OK)
		return (error);

	memory = scorpi_config_find_node(config, "memory");
	if (memory != NULL) {
		value = scorpi_config_get_value(memory, "wired");
		if (value != NULL) {
			error = scorpi_vm_set_inferred_prop(vm, "memory.wired",
			    value);
			if (error != SCORPI_OK)
				return (error);
		}
		value = scorpi_config_get_value(memory, "guest_in_core");
		if (value != NULL) {
			error = scorpi_vm_set_inferred_prop(vm,
			    "memory.guest_in_core", value);
			if (error != SCORPI_OK)
				return (error);
		}
	}

	value = scorpi_config_get_value(config, "virtio_msix");
	if (value != NULL) {
		error = scorpi_vm_set_inferred_prop(vm, "virtio_msix", value);
		if (error != SCORPI_OK)
			return (error);
	}

	value = scorpi_config_get_value(config, "acpi_tables");
	if (value != NULL) {
		error = scorpi_vm_set_inferred_prop(vm, "acpi_tables", value);
		if (error != SCORPI_OK)
			return (error);
	}

	value = scorpi_config_get_value(config, "acpi_tables_in_memory");
	if (value != NULL) {
		error = scorpi_vm_set_inferred_prop(vm, "acpi_tables_in_memory",
		    value);
		if (error != SCORPI_OK)
			return (error);
	}

	gic = scorpi_config_find_node(config, "gic");
	if (gic != NULL) {
		value = scorpi_config_get_value(gic, "msi");
		if (value != NULL) {
			error = scorpi_vm_set_inferred_prop(vm, "gic.msi", value);
			if (error != SCORPI_OK)
				return (error);
		}
	}

	error = scorpi_import_generic_vm_scalars(config, vm);
	if (error != SCORPI_OK)
		return (error);

	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_import_pci_devices(const nvlist_t *pci_root, scorpi_vm_t vm,
    struct scorpi_imported_xhci **out_xhcis, size_t *out_xhci_count)
{
	struct scorpi_imported_xhci imported_xhci;
	const nvlist_t *bus_node, *func_node, *slot_node;
	scorpi_device_t dev;
	void *bus_cookie, *func_cookie, *slot_cookie;
	const char *bus_name, *device_name, *func_name, *slot_name, *value;
	int bus_type, func_type, slot_type;
	scorpi_error_t error;
	uint64_t pci_bus, slot, func, usb_bus, next_usb_bus;

	*out_xhcis = NULL;
	*out_xhci_count = 0;
	next_usb_bus = 0;

	if (pci_root == NULL)
		return (SCORPI_OK);

	bus_cookie = NULL;
	while ((bus_name = nvlist_next(pci_root, &bus_type, &bus_cookie)) != NULL) {
		if (bus_type != NV_TYPE_NVLIST ||
		    !scorpi_parse_u64_string(bus_name, &pci_bus))
			return (SCORPI_ERR_UNSUPPORTED);
		if (pci_bus != 0)
			return (SCORPI_ERR_UNSUPPORTED);

		bus_node = nvlist_get_nvlist(pci_root, bus_name);
		slot_cookie = NULL;
		while ((slot_name = nvlist_next(bus_node, &slot_type,
		    &slot_cookie)) != NULL) {
			if (slot_type != NV_TYPE_NVLIST ||
			    !scorpi_parse_u64_string(slot_name, &slot))
				return (SCORPI_ERR_UNSUPPORTED);

			slot_node = nvlist_get_nvlist(bus_node, slot_name);
			func_cookie = NULL;
			while ((func_name = nvlist_next(slot_node, &func_type,
			    &func_cookie)) != NULL) {
				if (func_type != NV_TYPE_NVLIST ||
				    !scorpi_parse_u64_string(func_name, &func))
					return (SCORPI_ERR_UNSUPPORTED);
				if (func != 0)
					return (SCORPI_ERR_UNSUPPORTED);

				func_node = nvlist_get_nvlist(slot_node, func_name);
				value = scorpi_config_get_value(func_node, "device");
				if (value == NULL)
					return (SCORPI_ERR_VALIDATION);
				device_name = value;

				if (strcmp(device_name, "ahci") == 0) {
					error = scorpi_import_compatible_ahci_device(
					    func_node, slot, vm);
					if (error != SCORPI_OK)
						return (error);
					continue;
				}

				error = scorpi_create_pci_device(device_name, slot, &dev);
				if (error != SCORPI_OK)
					return (error);

				error = scorpi_import_device_props(func_node, dev,
				    "device", NULL);
				if (error != SCORPI_OK) {
					scorpi_destroy_device(dev);
					return (error);
				}

				error = scorpi_vm_add_device(vm, dev);
				if (error != SCORPI_OK) {
					scorpi_destroy_device(dev);
					return (error);
				}

				if (strcmp(device_name, "xhci") != 0)
					continue;

				value = scorpi_config_get_value(func_node, "bus");
				if (value != NULL && scorpi_parse_u64_string(value,
				    &usb_bus)) {
					if (usb_bus >= next_usb_bus)
						next_usb_bus = usb_bus + 1;
				} else {
					usb_bus = next_usb_bus++;
				}

				imported_xhci.dev = dev;
				imported_xhci.usb_bus = usb_bus;
				imported_xhci.pci_bus = (unsigned int)pci_bus;
				imported_xhci.slot = (unsigned int)slot;
				imported_xhci.func = (unsigned int)func;
				error = scorpi_add_imported_xhci(out_xhcis,
				    out_xhci_count, &imported_xhci);
				if (error != SCORPI_OK)
					return (error);
			}
		}
	}

	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_import_usb_devices(const nvlist_t *usb_root, scorpi_vm_t vm,
    struct scorpi_imported_xhci *xhcis, size_t xhci_count)
{
	const nvlist_t *bus_node, *slot_node;
	scorpi_device_t dev;
	struct scorpi_imported_xhci *parent_xhci;
	void *bus_cookie, *slot_cookie;
	const char *bus_name, *device_name, *slot_name;
	int bus_type, slot_type;
	scorpi_error_t error;
	uint64_t usb_bus, usb_slot;

	if (usb_root == NULL)
		return (SCORPI_OK);
	if (xhci_count == 0)
		return (SCORPI_ERR_VALIDATION);

	bus_cookie = NULL;
	while ((bus_name = nvlist_next(usb_root, &bus_type, &bus_cookie)) != NULL) {
		if (bus_type != NV_TYPE_NVLIST ||
		    !scorpi_parse_u64_string(bus_name, &usb_bus))
			return (SCORPI_ERR_UNSUPPORTED);

		bus_node = nvlist_get_nvlist(usb_root, bus_name);
		slot_cookie = NULL;
		while ((slot_name = nvlist_next(bus_node, &slot_type,
		    &slot_cookie)) != NULL) {
			if (slot_type != NV_TYPE_NVLIST ||
			    !scorpi_parse_u64_string(slot_name, &usb_slot))
				return (SCORPI_ERR_UNSUPPORTED);

			slot_node = nvlist_get_nvlist(bus_node, slot_name);
			device_name = scorpi_config_get_value(slot_node, "device");
			if (device_name == NULL)
				return (SCORPI_ERR_VALIDATION);

			error = scorpi_create_usb_device(device_name, &dev);
			if (error != SCORPI_OK)
				return (error);

			error = scorpi_import_device_props(slot_node, dev, "device",
			    NULL);
			if (error != SCORPI_OK) {
				scorpi_destroy_device(dev);
				return (error);
			}

			error = scorpi_device_set_prop_u64(dev, "port", usb_slot);
			if (error != SCORPI_OK) {
				scorpi_destroy_device(dev);
				return (error);
			}

			if (xhci_count == 1) {
				if (xhcis[0].usb_bus != usb_bus) {
					scorpi_destroy_device(dev);
					return (SCORPI_ERR_INVALID_PARENT);
				}
			} else {
				const char *parent_id;

				parent_xhci = scorpi_find_imported_xhci_by_usb_bus(
				    xhcis, xhci_count, usb_bus);
				if (parent_xhci == NULL) {
					scorpi_destroy_device(dev);
					return (SCORPI_ERR_INVALID_PARENT);
				}
				error = scorpi_assign_imported_xhci_id(parent_xhci);
				if (error != SCORPI_OK) {
					scorpi_destroy_device(dev);
					return (error);
				}
				parent_id = scorpi_imported_xhci_id(parent_xhci);
				if (parent_id == NULL) {
					scorpi_destroy_device(dev);
					return (SCORPI_ERR_RUNTIME);
				}
				error = scorpi_device_set_prop_string(dev, "parent",
				    parent_id);
				if (error != SCORPI_OK) {
					scorpi_destroy_device(dev);
					return (error);
				}
			}

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
scorpi_import_lpc_devices(const nvlist_t *config, scorpi_vm_t vm)
{
	const nvlist_t *tpm;
	scorpi_device_t dev;
	const char *path;
	scorpi_error_t error;

	path = scorpi_config_get_value(config, "comm_sock");
	if (path != NULL) {
		error = scorpi_create_lpc_device("vm-control", &dev);
		if (error != SCORPI_OK)
			return (error);
		error = scorpi_device_set_prop_string(dev, "path", path);
		if (error != SCORPI_OK) {
			scorpi_destroy_device(dev);
			return (error);
		}
		error = scorpi_vm_add_device(vm, dev);
		if (error != SCORPI_OK) {
			scorpi_destroy_device(dev);
			return (error);
		}
	}

	tpm = scorpi_config_find_node(config, "tpm");
	if (tpm == NULL)
		return (SCORPI_OK);

	error = scorpi_create_lpc_device("tpm", &dev);
	if (error != SCORPI_OK)
		return (error);
	error = scorpi_import_device_props(tpm, dev, "device", NULL);
	if (error != SCORPI_OK) {
		scorpi_destroy_device(dev);
		return (error);
	}
	error = scorpi_vm_add_device(vm, dev);
	if (error != SCORPI_OK) {
		scorpi_destroy_device(dev);
		return (error);
	}
	return (SCORPI_OK);
}

scorpi_error_t
scorpi_load_vm_from_config_tree(const nvlist_t *config, scorpi_vm_t *out_vm)
{
	struct scorpi_imported_xhci *xhcis;
	scorpi_vm_t vm;
	scorpi_error_t error;
	size_t xhci_count;

	if (config == NULL || out_vm == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	*out_vm = NULL;

	error = scorpi_create_vm(&vm);
	if (error != SCORPI_OK)
		return (error);

	xhcis = NULL;
	xhci_count = 0;

	error = scorpi_import_vm_scalars(config, vm);
	if (error != SCORPI_OK)
		goto fail;
	error = scorpi_import_pci_devices(scorpi_config_find_node(config, "pci"),
	    vm, &xhcis, &xhci_count);
	if (error != SCORPI_OK)
		goto fail;
	error = scorpi_import_usb_devices(scorpi_config_find_node(config, "usb"),
	    vm, xhcis, xhci_count);
	if (error != SCORPI_OK)
		goto fail;
	error = scorpi_import_lpc_devices(config, vm);
	if (error != SCORPI_OK)
		goto fail;

	free(xhcis);
	*out_vm = vm;
	return (SCORPI_OK);

fail:
	free(xhcis);
	scorpi_destroy_vm(vm);
	return (error);
}

struct scorpi_usb_bus_binding {
	const char *id;
	uint64_t bus;
	uint64_t next_slot;
};

static scorpi_error_t
scorpi_collect_usb_bus_bindings(const struct scorpi_normalized_vm *vm,
    struct scorpi_usb_bus_binding **out_bindings, size_t *out_count)
{
	const struct scorpi_normalized_device *device;
	const struct scorpi_normalized_prop *prop;
	struct scorpi_usb_bus_binding *bindings;
	size_t count, i;
	uint64_t next_bus;

	*out_bindings = NULL;
	*out_count = 0;

	count = 0;
	for (i = 0; i < vm->device_count; i++) {
		if (vm->devices[i].bus == SCORPI_BUS_PCI &&
		    strcmp(vm->devices[i].device, "xhci") == 0)
			count++;
	}
	if (count == 0)
		return (SCORPI_OK);

	bindings = calloc(count, sizeof(*bindings));
	if (bindings == NULL)
		return (SCORPI_ERR_RUNTIME);

	next_bus = 0;
	count = 0;
	for (i = 0; i < vm->device_count; i++) {
		device = &vm->devices[i];
		if (device->bus != SCORPI_BUS_PCI || strcmp(device->device, "xhci") != 0)
			continue;

		bindings[count].id = device->id;
		bindings[count].next_slot = 1;
		prop = scorpi_normalized_device_find_prop(device, "bus");
		if (prop != NULL && prop->kind == SCORPI_PROP_U64)
			bindings[count].bus = prop->value.u64;
		else
			bindings[count].bus = next_bus++;
		if (bindings[count].bus >= next_bus)
			next_bus = bindings[count].bus + 1;
		count++;
	}

	*out_bindings = bindings;
	*out_count = count;
	return (SCORPI_OK);
}

static struct scorpi_usb_bus_binding *
scorpi_find_usb_bus_binding(struct scorpi_usb_bus_binding *bindings, size_t count,
    const char *id)
{
	size_t i;

	if (count == 1 && id == NULL)
		return (&bindings[0]);
	if (id == NULL)
		return (NULL);
	for (i = 0; i < count; i++) {
		if (bindings[i].id != NULL && strcmp(bindings[i].id, id) == 0)
			return (&bindings[i]);
	}
	return (NULL);
}

static scorpi_error_t
scorpi_vm_props_to_config(const struct scorpi_normalized_vm *vm, nvlist_t *config)
{
	const struct scorpi_normalized_prop *prop;
	char *value;
	size_t i;
	scorpi_error_t error;

	for (i = 0; i < vm->prop_count; i++) {
		prop = &vm->props[i];
		if (strcmp(prop->name, "cpu.cores") == 0) {
			error = scorpi_normalized_prop_to_string(prop, &value);
			if (error != SCORPI_OK)
				return (error);
			error = scorpi_config_set_value(config, "cpus", value);
			if (error == SCORPI_OK)
				error = scorpi_config_set_value(config, "cores", value);
			free(value);
			if (error != SCORPI_OK)
				return (error);
			error = scorpi_config_set_value(config, "sockets", "1");
			if (error == SCORPI_OK)
				error = scorpi_config_set_value(config, "threads", "1");
			if (error != SCORPI_OK)
				return (error);
			continue;
		}

		error = scorpi_normalized_prop_to_string(prop, &value);
		if (error != SCORPI_OK)
			return (error);
		error = scorpi_config_set_value(config, prop->name, value);
		free(value);
		if (error != SCORPI_OK)
			return (error);
	}

	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_device_props_to_config(const struct scorpi_normalized_device *device,
    nvlist_t *config, const char *base_path, bool skip_identity_props)
{
	const struct scorpi_normalized_prop *prop;
	char *full_path, *value;
	size_t i, path_len;
	scorpi_error_t error;

	for (i = 0; i < device->prop_count; i++) {
		prop = &device->props[i];
		if (strcmp(prop->name, "id") == 0 || strcmp(prop->name, "parent") == 0)
			continue;
		if (skip_identity_props && strcmp(prop->name, "bus") == 0)
			continue;
		if (skip_identity_props && strcmp(prop->name, "port") == 0)
			continue;

		error = scorpi_normalized_prop_to_string(prop, &value);
		if (error != SCORPI_OK)
			return (error);

		path_len = strlen(base_path) + 1 + strlen(prop->name) + 1;
		full_path = calloc(path_len, 1);
		if (full_path == NULL) {
			free(value);
			return (SCORPI_ERR_RUNTIME);
		}
		snprintf(full_path, path_len, "%s.%s", base_path, prop->name);
		error = scorpi_config_set_value(config, full_path, value);
		free(full_path);
		free(value);
		if (error != SCORPI_OK)
			return (error);
	}

	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_pci_device_to_config(const struct scorpi_normalized_device *device,
    struct scorpi_usb_bus_binding *bindings, size_t binding_count,
    nvlist_t *config)
{
	const char *ahci_type;
	const struct scorpi_normalized_prop *bus_prop;
	char base_path[64];
	char full_path[80];
	char bus_value[32];
	scorpi_error_t error;
	size_t i;

	snprintf(base_path, sizeof(base_path), "pci.0.%llu.0",
	    (unsigned long long)device->slot);
	if (scorpi_legacy_ahci_device_type(device->device, &ahci_type)) {
		char port_path[96];

		snprintf(full_path, sizeof(full_path), "%s.device", base_path);
		error = scorpi_config_set_value(config, full_path, "ahci");
		if (error != SCORPI_OK)
			return (error);

		bus_prop = scorpi_normalized_device_find_prop(device, "bus");
		if (bus_prop != NULL && bus_prop->kind == SCORPI_PROP_U64) {
			snprintf(bus_value, sizeof(bus_value), "%llu",
			    (unsigned long long)bus_prop->value.u64);
			snprintf(full_path, sizeof(full_path), "%s.bus", base_path);
			error = scorpi_config_set_value(config, full_path, bus_value);
			if (error != SCORPI_OK)
				return (error);
		}

		snprintf(port_path, sizeof(port_path), "%s.port.0", base_path);
		snprintf(full_path, sizeof(full_path), "%s.type", port_path);
		error = scorpi_config_set_value(config, full_path, ahci_type);
		if (error != SCORPI_OK)
			return (error);
		return (scorpi_device_props_to_config(device, config, port_path,
		    true));
	}

	snprintf(full_path, sizeof(full_path), "%s.device", base_path);
	error = scorpi_config_set_value(config, full_path, device->device);
	if (error != SCORPI_OK)
		return (error);

	bus_prop = scorpi_normalized_device_find_prop(device, "bus");
	if (bus_prop != NULL && bus_prop->kind == SCORPI_PROP_U64) {
		snprintf(bus_value, sizeof(bus_value), "%llu",
		    (unsigned long long)bus_prop->value.u64);
		snprintf(full_path, sizeof(full_path), "%s.bus", base_path);
		error = scorpi_config_set_value(config, full_path, bus_value);
		if (error != SCORPI_OK)
			return (error);
	} else if (binding_count > 1) {
		for (i = 0; i < binding_count; i++) {
			if (bindings[i].id != NULL && device->id != NULL &&
			    strcmp(bindings[i].id, device->id) == 0) {
				snprintf(bus_value, sizeof(bus_value), "%llu",
				    (unsigned long long)bindings[i].bus);
				snprintf(full_path, sizeof(full_path), "%s.bus",
				    base_path);
				error = scorpi_config_set_value(config, full_path,
				    bus_value);
				if (error != SCORPI_OK)
					return (error);
				break;
			}
		}
	}

	return (scorpi_device_props_to_config(device, config, base_path, true));
}

static scorpi_error_t
scorpi_usb_device_to_config(const struct scorpi_normalized_device *device,
    struct scorpi_usb_bus_binding *bindings, size_t binding_count,
    nvlist_t *config)
{
	const struct scorpi_normalized_prop *port_prop;
	struct scorpi_usb_bus_binding *binding;
	char base_path[64];
	char full_path[80];
	uint64_t usb_slot;
	scorpi_error_t error;

	binding = scorpi_find_usb_bus_binding(bindings, binding_count,
	    device->parent);
	if (binding == NULL)
		return (SCORPI_ERR_VALIDATION);

	port_prop = scorpi_normalized_device_find_prop(device, "port");
	if (port_prop != NULL && port_prop->kind == SCORPI_PROP_U64)
		usb_slot = port_prop->value.u64;
	else
		usb_slot = binding->next_slot++;

	snprintf(base_path, sizeof(base_path), "usb.%llu.%llu",
	    (unsigned long long)binding->bus, (unsigned long long)usb_slot);
	snprintf(full_path, sizeof(full_path), "%s.device", base_path);
	error = scorpi_config_set_value(config, full_path, device->device);
	if (error != SCORPI_OK)
		return (error);

	return (scorpi_device_props_to_config(device, config, base_path, true));
}

static scorpi_error_t
scorpi_lpc_device_to_config(const struct scorpi_normalized_device *device,
    nvlist_t *config)
{
	char base_path[32];
	scorpi_error_t error;

	if (strcmp(device->device, "vm-control") == 0) {
		const struct scorpi_normalized_prop *path_prop;

		path_prop = scorpi_normalized_device_find_prop(device, "path");
		if (path_prop == NULL)
			return (SCORPI_ERR_VALIDATION);
		if (path_prop->kind != SCORPI_PROP_STRING ||
		    path_prop->value.string == NULL)
			return (SCORPI_ERR_VALIDATION);
		return (scorpi_config_set_value(config, "comm_sock",
		    path_prop->value.string));
	}

	if (strcmp(device->device, "tpm") != 0)
		return (SCORPI_ERR_UNSUPPORTED_DEVICE);

	snprintf(base_path, sizeof(base_path), "tpm");
	error = scorpi_config_set_value(config, "tpm.device", "tpm");
	if (error != SCORPI_OK)
		return (error);
	return (scorpi_device_props_to_config(device, config, base_path, false));
}

scorpi_error_t
scorpi_vm_to_config(const struct scorpi_normalized_vm *vm, nvlist_t **out_config)
{
	struct scorpi_usb_bus_binding *bindings;
	nvlist_t *config;
	scorpi_error_t error;
	size_t binding_count, i;

	if (vm == NULL || out_config == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	*out_config = NULL;

	config = nvlist_create(0);
	if (config == NULL)
		return (SCORPI_ERR_RUNTIME);

	error = scorpi_vm_props_to_config(vm, config);
	if (error != SCORPI_OK)
		goto fail;

	error = scorpi_collect_usb_bus_bindings(vm, &bindings, &binding_count);
	if (error != SCORPI_OK)
		goto fail;

	for (i = 0; i < vm->device_count; i++) {
		switch (vm->devices[i].bus) {
		case SCORPI_BUS_PCI:
			error = scorpi_pci_device_to_config(&vm->devices[i], bindings,
			    binding_count, config);
			break;
		case SCORPI_BUS_USB:
			error = scorpi_usb_device_to_config(&vm->devices[i], bindings,
			    binding_count, config);
			break;
		case SCORPI_BUS_LPC:
			error = scorpi_lpc_device_to_config(&vm->devices[i], config);
			break;
		default:
			error = SCORPI_ERR_UNSUPPORTED_DEVICE;
			break;
		}
		if (error != SCORPI_OK) {
			free(bindings);
			goto fail;
		}
	}

	free(bindings);
	*out_config = config;
	return (SCORPI_OK);

fail:
	nvlist_destroy(config);
	return (error);
}

__attribute__((weak))
int
scorpi_runtime_run_child(const nvlist_t *config)
{
	(void)config;
	return (EX_UNAVAILABLE);
}

static scorpi_error_t
scorpi_vm_prepare_runtime_config(const struct scorpi_vm *vm, nvlist_t **out_config)
{
	struct scorpi_normalized_vm *normalized_vm;
	nvlist_t *config;
	char generated_name[64];
	scorpi_error_t error;

	if (vm == NULL || out_config == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	*out_config = NULL;

	error = scorpi_vm_validate(vm);
	if (error != SCORPI_OK)
		return (error);

	error = scorpi_vm_normalize(vm, &normalized_vm);
	if (error != SCORPI_OK)
		return (error);

	error = scorpi_vm_to_config(normalized_vm, &config);
	scorpi_normalized_vm_destroy(normalized_vm);
	if (error != SCORPI_OK)
		return (error);

	if (scorpi_config_get_value(config, "name") == NULL) {
		snprintf(generated_name, sizeof(generated_name), "scorpi-%ld-%p",
		    (long)getpid(), (const void *)vm);
		error = scorpi_config_set_value(config, "name", generated_name);
		if (error != SCORPI_OK) {
			scorpi_config_destroy(config);
			return (error);
		}
	}

	*out_config = config;
	return (SCORPI_OK);
}

static bool
scorpi_prop_is_nonempty_string(const struct scorpi_prop *prop)
{
	return (prop != NULL && prop->kind == SCORPI_PROP_STRING &&
	    prop->value.string != NULL && *prop->value.string != '\0');
}

static bool
scorpi_device_has_string_prop(const struct scorpi_device *dev, const char *name)
{
	return (scorpi_prop_is_nonempty_string(scorpi_device_find_prop(dev, name)));
}

static size_t
scorpi_count_xhci_controllers(const struct scorpi_vm *vm)
{
	const struct scorpi_device *dev;
	size_t count;

	count = 0;
	for (dev = vm->devices; dev != NULL; dev = dev->next) {
		if (dev->bus == SCORPI_BUS_PCI && strcmp(dev->device, "xhci") == 0)
			count++;
	}

	return (count);
}

static scorpi_error_t
scorpi_validate_vm_required_props(const struct scorpi_vm *vm)
{
	const struct scorpi_prop *cpu_prop;
	const struct scorpi_prop *memory_prop;

	cpu_prop = scorpi_vm_find_prop(vm, "cpu.cores");
	if (cpu_prop == NULL || cpu_prop->kind != SCORPI_PROP_U64 ||
	    cpu_prop->value.u64 == 0)
		return (SCORPI_ERR_VALIDATION);

	memory_prop = scorpi_vm_find_prop(vm, "memory.size");
	if (memory_prop == NULL || memory_prop->kind != SCORPI_PROP_U64 ||
	    memory_prop->value.u64 == 0)
		return (SCORPI_ERR_VALIDATION);

	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_validate_device_ids(const struct scorpi_vm *vm)
{
	const struct scorpi_device *dev, *other;
	const char *id, *other_id;
	const struct scorpi_prop *id_prop;

	for (dev = vm->devices; dev != NULL; dev = dev->next) {
		id_prop = scorpi_device_find_prop(dev, "id");
		if (id_prop != NULL && !scorpi_prop_is_nonempty_string(id_prop))
			return (SCORPI_ERR_VALIDATION);

		id = scorpi_device_get_id(dev);
		if (id == NULL)
			continue;

		for (other = dev->next; other != NULL; other = other->next) {
			other_id = scorpi_device_get_id(other);
			if (other_id != NULL && strcmp(id, other_id) == 0)
				return (SCORPI_ERR_DUPLICATE_ID);
		}
	}

	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_validate_pci_slots(const struct scorpi_vm *vm)
{
	const struct scorpi_device *dev, *other;

	for (dev = vm->devices; dev != NULL; dev = dev->next) {
		if (dev->bus != SCORPI_BUS_PCI || dev->slot == SCORPI_PCI_SLOT_AUTO)
			continue;

		for (other = dev->next; other != NULL; other = other->next) {
			if (other->bus == SCORPI_BUS_PCI && other->slot == dev->slot)
				return (SCORPI_ERR_VALIDATION);
		}
	}

	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_validate_usb_devices(const struct scorpi_vm *vm)
{
	const struct scorpi_device *dev, *parent;
	size_t xhci_count;
	scorpi_error_t error;

	xhci_count = scorpi_count_xhci_controllers(vm);
	for (dev = vm->devices; dev != NULL; dev = dev->next) {
		if (dev->bus != SCORPI_BUS_USB)
			continue;

		error = scorpi_vm_resolve_parent(vm, dev, &parent);
		if (error != SCORPI_OK)
			return (error);
		if (parent != NULL) {
			if (parent->bus != SCORPI_BUS_PCI ||
			    strcmp(parent->device, "xhci") != 0)
				return (SCORPI_ERR_VALIDATION);
			continue;
		}

		if (xhci_count != 1)
			return (SCORPI_ERR_VALIDATION);
	}

	return (SCORPI_OK);
}

static scorpi_error_t
scorpi_validate_lpc_device(const struct scorpi_device *dev)
{
	if (dev->bus != SCORPI_BUS_LPC)
		return (SCORPI_OK);

	if (strcmp(dev->device, "vm-control") == 0) {
		if (!scorpi_device_has_string_prop(dev, "path"))
			return (SCORPI_ERR_VALIDATION);
		return (SCORPI_OK);
	}

	if (strcmp(dev->device, "tpm") == 0) {
		if (!scorpi_device_has_string_prop(dev, "type") ||
		    !scorpi_device_has_string_prop(dev, "path") ||
		    !scorpi_device_has_string_prop(dev, "version") ||
		    !scorpi_device_has_string_prop(dev, "intf"))
			return (SCORPI_ERR_VALIDATION);
	}

	return (SCORPI_OK);
}

scorpi_error_t
scorpi_vm_validate(const struct scorpi_vm *vm)
{
	const struct scorpi_device *dev, *parent;
	scorpi_error_t error;

	if (vm == NULL)
		return (SCORPI_ERR_INVALID_ARG);

	error = scorpi_validate_vm_required_props(vm);
	if (error != SCORPI_OK)
		return (error);

	error = scorpi_validate_device_ids(vm);
	if (error != SCORPI_OK)
		return (error);

	error = scorpi_validate_pci_slots(vm);
	if (error != SCORPI_OK)
		return (error);

	error = scorpi_validate_usb_devices(vm);
	if (error != SCORPI_OK)
		return (error);

	for (dev = vm->devices; dev != NULL; dev = dev->next) {
		error = scorpi_vm_resolve_parent(vm, dev, &parent);
		if (error != SCORPI_OK)
			return (error);
		error = scorpi_validate_lpc_device(dev);
		if (error != SCORPI_OK)
			return (error);
	}

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
	int status;

	if (vm == NULL)
		return;

	if (vm->state == SCORPI_VM_RUNNING) {
		(void)kill(vm->child_pid, SIGTERM);
		(void)waitpid(vm->child_pid, &status, 0);
	}

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
	nvlist_t *config;
	pid_t pid;
	scorpi_error_t error;

	if (vm == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	if (vm->state == SCORPI_VM_RUNNING)
		return (SCORPI_ERR_INVALID_ARG);

	error = scorpi_vm_prepare_runtime_config(vm, &config);
	if (error != SCORPI_OK)
		return (error);

	pid = fork();
	if (pid < 0) {
		scorpi_config_destroy(config);
		return (SCORPI_ERR_RUNTIME);
	}

	if (pid == 0) {
		int child_exit_code;

		child_exit_code = scorpi_runtime_run_child(config);
		scorpi_config_destroy(config);
		_exit(child_exit_code);
	}

	scorpi_config_destroy(config);
	vm->child_pid = pid;
	vm->state = SCORPI_VM_RUNNING;
	vm->exit_code = 0;
	return (SCORPI_OK);
}

scorpi_error_t
scorpi_wait_vm(scorpi_vm_t vm, int *exit_code)
{
	int status;
	pid_t waited;

	if (vm == NULL || exit_code == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	if (vm->state == SCORPI_VM_EXITED) {
		*exit_code = vm->exit_code;
		return (SCORPI_OK);
	}
	if (vm->state != SCORPI_VM_RUNNING)
		return (SCORPI_ERR_INVALID_ARG);

	waited = waitpid(vm->child_pid, &status, 0);
	if (waited < 0)
		return (SCORPI_ERR_RUNTIME);

	if (WIFEXITED(status))
		vm->exit_code = WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		vm->exit_code = 128 + WTERMSIG(status);
	else
		vm->exit_code = 1;

	vm->state = SCORPI_VM_EXITED;
	vm->child_pid = 0;
	*exit_code = vm->exit_code;
	return (SCORPI_OK);
}

scorpi_error_t
scorpi_stop_vm(scorpi_vm_t vm)
{
	if (vm == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	if (vm->state != SCORPI_VM_RUNNING)
		return (SCORPI_ERR_INVALID_ARG);
	if (kill(vm->child_pid, SIGTERM) != 0)
		return (SCORPI_ERR_RUNTIME);
	return (SCORPI_OK);
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
