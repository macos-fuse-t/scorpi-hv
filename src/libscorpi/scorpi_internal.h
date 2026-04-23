#pragma once

#include <stdbool.h>
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

const struct scorpi_prop *scorpi_vm_find_prop(const scorpi_vm_t *vm,
    const char *name);
const struct scorpi_prop *scorpi_device_find_prop(const scorpi_device_t *dev,
    const char *name);
const scorpi_device_t *scorpi_vm_find_device_by_id(const scorpi_vm_t *vm,
    const char *id);
scorpi_error_t scorpi_vm_resolve_parent(const scorpi_vm_t *vm,
    const scorpi_device_t *dev, const scorpi_device_t **out_parent);
