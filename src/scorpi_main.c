/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <err.h>
#include <stdio.h>
#include <sysexits.h>
#include <unistd.h>

#include "bhyverun.h"
#include "config.h"
#include "nv.h"
#include "scorpi_cli.h"
#include "scorpi_internal.h"

static int
bhyve_run_vm_with_scorpi_kit(scorpi_vm_t vm)
{
	int exit_code;

	exit_code = scorpi_start_vm(vm);
	if (exit_code < 0) {
		fprintf(stderr, "scorpi_kit: failed to start VM (%d)\n",
		    -exit_code);
		scorpi_destroy_vm(vm);
		return (1);
	}

	scorpi_destroy_vm(vm);
	return (exit_code);
}

static int
bhyve_run_via_scorpi_kit(void)
{
	const nvlist_t *config;
	scorpi_vm_t vm;
	scorpi_error_t error;

	config = get_config_tree();
	error = scorpi_load_vm_from_config_tree(config, &vm);
	if (error == SCORPI_ERR_UNSUPPORTED)
		return (bhyve_run_configured_vm());
	if (error != SCORPI_OK) {
		fprintf(stderr, "scorpi_kit: failed to translate CLI config (%d)\n",
		    error);
		return (1);
	}

	return (bhyve_run_vm_with_scorpi_kit(vm));
}

static int
bhyve_run_yaml_file(const char *path)
{
	scorpi_vm_t vm;
	scorpi_error_t error;

	error = scorpi_cli_load_vm_from_yaml_file(path, &vm);
	if (error != SCORPI_OK) {
		fprintf(stderr, "scorpi_kit: failed to load YAML file %s (%d)\n",
		    path, error);
		return (1);
	}

	return (bhyve_run_vm_with_scorpi_kit(vm));
}

int
main(int argc, char *argv[])
{
	const char *yaml_file;

	bhyve_init_config();
	bhyve_optparse(argc, argv);
	argc -= optind;
	argv += optind;

	yaml_file = bhyve_get_yaml_config_file();
	if (yaml_file != NULL) {
		if (argc != 0)
			errx(EX_USAGE, "-f cannot be combined with a vmname");
		if (bhyve_legacy_config_used())
			errx(EX_USAGE,
			    "-f cannot be combined with legacy configuration options");
		return (bhyve_run_yaml_file(yaml_file));
	}

	if (argc > 1)
		bhyve_usage(1);

	if (argc == 1)
		set_config_value("name", argv[0]);
	else if (argc == 0 && get_config_value("name") == NULL)
		bhyve_usage(1);

	return (bhyve_run_via_scorpi_kit());
}
