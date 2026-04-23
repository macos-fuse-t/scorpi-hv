/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stddef.h>
#include <stdint.h>

#include "bhyverun.h"
#include "config.h"
#include "nv.h"

int
scorpi_runtime_run_child(const nvlist_t *config)
{
	bhyve_init_config();
	merge_config_tree(config);
	return (bhyve_run_configured_vm());
}
