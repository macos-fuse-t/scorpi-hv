/* CLI-side helpers that stay outside the public scorpi_kit API surface. */

#include <stdio.h>
#include <stdlib.h>

#include "scorpi_cli.h"

scorpi_error_t
scorpi_cli_load_vm_from_yaml_file(const char *path, scorpi_vm_t *out_vm)
{
	char *yaml;
	long size;
	size_t read_size;
	FILE *fp;

	if (path == NULL || out_vm == NULL)
		return (SCORPI_ERR_INVALID_ARG);
	*out_vm = NULL;

	fp = fopen(path, "rb");
	if (fp == NULL)
		return (SCORPI_ERR_RUNTIME);

	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return (SCORPI_ERR_RUNTIME);
	}
	size = ftell(fp);
	if (size < 0) {
		fclose(fp);
		return (SCORPI_ERR_RUNTIME);
	}
	if (fseek(fp, 0, SEEK_SET) != 0) {
		fclose(fp);
		return (SCORPI_ERR_RUNTIME);
	}

	yaml = calloc((size_t)size + 1, 1);
	if (yaml == NULL) {
		fclose(fp);
		return (SCORPI_ERR_RUNTIME);
	}

	read_size = fread(yaml, 1, (size_t)size, fp);
	fclose(fp);
	if (read_size != (size_t)size) {
		free(yaml);
		return (SCORPI_ERR_RUNTIME);
	}

	yaml[size] = '\0';
	{
		scorpi_error_t error;

		error = scorpi_load_vm_from_yaml(yaml, out_vm);
		free(yaml);
		return (error);
	}
}
