/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "scorpi.h"

#define GIB ((uint64_t)1024 * 1024 * 1024)

static int
fail(const char *what, scorpi_error_t error)
{
	fprintf(stderr, "%s failed: %d\n", what, error);
	return (1);
}

int
main(void)
{
	scorpi_device_t dev;
	scorpi_vm_t vm;
	scorpi_error_t error;
	int exit_code;

	error = scorpi_create_vm(&vm);
	if (error != SCORPI_OK)
		return (fail("scorpi_create_vm", error));

	error = scorpi_vm_set_prop_string(vm, "name", "freebsd-api-sample");
	if (error != SCORPI_OK)
		goto fail_vm;
	error = scorpi_vm_set_prop_string(vm, "console", "stdio");
	if (error != SCORPI_OK)
		goto fail_vm;
	error = scorpi_vm_set_prop_string(vm, "bootrom", "./firmware/u-boot.bin");
	if (error != SCORPI_OK)
		goto fail_vm;
	error = scorpi_vm_set_cpu(vm, 4);
	if (error != SCORPI_OK)
		goto fail_vm;
	error = scorpi_vm_set_ram(vm, 2 * GIB);
	if (error != SCORPI_OK)
		goto fail_vm;

	error = scorpi_create_pci_device("hostbridge", 0, &dev);
	if (error != SCORPI_OK)
		goto fail_vm;
	error = scorpi_vm_add_device(vm, dev);
	if (error != SCORPI_OK) {
		scorpi_destroy_device(dev);
		goto fail_vm;
	}

	error = scorpi_create_pci_device("virtio-net", 1, &dev);
	if (error != SCORPI_OK)
		goto fail_vm;
	error = scorpi_device_set_prop_string(dev, "backend", "slirp");
	if (error != SCORPI_OK) {
		scorpi_destroy_device(dev);
		goto fail_vm;
	}
	error = scorpi_vm_add_device(vm, dev);
	if (error != SCORPI_OK) {
		scorpi_destroy_device(dev);
		goto fail_vm;
	}

	error = scorpi_create_pci_device("virtio-blk", 2, &dev);
	if (error != SCORPI_OK)
		goto fail_vm;
	error = scorpi_device_set_prop_string(
	    dev, "path", "./FreeBSD-14.2-RELEASE-arm64-aarch64-bootonly.iso");
	if (error != SCORPI_OK) {
		scorpi_destroy_device(dev);
		goto fail_vm;
	}
	error = scorpi_device_set_prop_bool(dev, "ro", true);
	if (error != SCORPI_OK) {
		scorpi_destroy_device(dev);
		goto fail_vm;
	}
	error = scorpi_vm_add_device(vm, dev);
	if (error != SCORPI_OK) {
		scorpi_destroy_device(dev);
		goto fail_vm;
	}

	exit_code = scorpi_start_vm(vm);
	if (exit_code < 0) {
		error = (scorpi_error_t)-exit_code;
		goto fail_vm;
	}

	scorpi_destroy_vm(vm);
	return (exit_code);

fail_vm:
	(void)fail("freebsd_api_sample", error);
	scorpi_destroy_vm(vm);
	return (1);
}
