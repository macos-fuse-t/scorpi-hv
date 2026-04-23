/* YAML shorthand and expanded-form normalization coverage. */

#include <assert.h>
#include <stddef.h>

#include "scorpi_internal.h"

int
main(void)
{
	static const char shorthand_yaml[] =
	    "cpu: 2\n"
	    "memory: 4G\n";
	static const char expanded_yaml[] =
	    "cpu:\n"
	    "  cores: 2\n"
	    "memory:\n"
	    "  size: 4G\n";
	struct scorpi_normalized_vm *expanded_norm;
	struct scorpi_normalized_vm *shorthand_norm;
	scorpi_vm_t expanded_vm;
	scorpi_vm_t shorthand_vm;

	assert(scorpi_load_vm_from_yaml(shorthand_yaml, &shorthand_vm) ==
	    SCORPI_OK);
	assert(scorpi_load_vm_from_yaml(expanded_yaml, &expanded_vm) ==
	    SCORPI_OK);

	assert(scorpi_vm_normalize(shorthand_vm, &shorthand_norm) == SCORPI_OK);
	assert(scorpi_vm_normalize(expanded_vm, &expanded_norm) == SCORPI_OK);
	assert(scorpi_normalized_vm_equal(shorthand_norm, expanded_norm) == true);

	scorpi_normalized_vm_destroy(expanded_norm);
	scorpi_normalized_vm_destroy(shorthand_norm);
	scorpi_destroy_vm(expanded_vm);
	scorpi_destroy_vm(shorthand_vm);
	return (0);
}
