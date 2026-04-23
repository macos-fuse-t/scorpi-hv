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
};

struct scorpi_device {
	char *device;
	uint64_t slot;
	int bus;
};

const struct scorpi_prop *scorpi_vm_find_prop(const scorpi_vm_t *vm,
    const char *name);
