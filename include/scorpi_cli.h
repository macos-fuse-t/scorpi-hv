#pragma once

#include "scorpi.h"

scorpi_error_t scorpi_cli_load_vm_from_yaml_file(const char *path,
    scorpi_vm_t *out_vm);
