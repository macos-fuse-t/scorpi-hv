#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scorpi.h"

enum scorpi_prop_kind {
	SCORPI_PROP_STRING = 0,
	SCORPI_PROP_BOOL,
	SCORPI_PROP_U64,
};

struct scorpi_prop {
	struct scorpi_prop *next;
	char *name;
	enum scorpi_prop_kind kind;
	union {
		char *string;
		bool boolean;
		uint64_t u64;
	} value;
};

struct scorpi_vm {
	struct scorpi_prop *props;
	struct scorpi_device *devices;
};

struct scorpi_device {
	struct scorpi_device *next;
	char *device;
	struct scorpi_prop *props;
	uint64_t slot;
	int bus;
	bool attached;
};

struct scorpi_normalized_prop {
	char *name;
	enum scorpi_prop_kind kind;
	union {
		char *string;
		bool boolean;
		uint64_t u64;
	} value;
};

struct scorpi_normalized_device {
	char *device;
	char *id;
	char *parent;
	uint64_t slot;
	int bus;
	size_t prop_count;
	struct scorpi_normalized_prop *props;
};

struct scorpi_normalized_vm {
	size_t prop_count;
	struct scorpi_normalized_prop *props;
	size_t device_count;
	struct scorpi_normalized_device *devices;
};

const struct scorpi_prop *scorpi_vm_find_prop(const struct scorpi_vm *vm,
    const char *name);
const struct scorpi_prop *scorpi_device_find_prop(
    const struct scorpi_device *dev, const char *name);
const struct scorpi_device *scorpi_vm_find_device_by_id(
    const struct scorpi_vm *vm,
    const char *id);
scorpi_error_t scorpi_vm_resolve_parent(const struct scorpi_vm *vm,
    const struct scorpi_device *dev, const struct scorpi_device **out_parent);
scorpi_error_t scorpi_vm_validate(const struct scorpi_vm *vm);
scorpi_error_t scorpi_vm_normalize(const struct scorpi_vm *vm,
    struct scorpi_normalized_vm **out_vm);
void scorpi_normalized_vm_destroy(struct scorpi_normalized_vm *vm);
bool scorpi_normalized_vm_equal(const struct scorpi_normalized_vm *lhs,
    const struct scorpi_normalized_vm *rhs);
const struct scorpi_normalized_prop *scorpi_normalized_vm_find_prop(
    const struct scorpi_normalized_vm *vm, const char *name);
