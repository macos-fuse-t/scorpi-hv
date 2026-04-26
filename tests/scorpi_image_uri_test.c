/* Base location URI parser coverage for snapshot support. */

#include <sys/errno.h>
#include <sys/stat.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "scorpi_image_uri.h"

static void
mkdir_or_exists(const char *path)
{
	assert(mkdir(path, 0700) == 0 || errno == EEXIST);
}

static void
expect_resolved(const char *child, const char *uri,
    const struct scorpi_image_uri_policy *policy, const char *expected)
{
	struct scorpi_image_base_location *location;

	location = NULL;
	assert(scorpi_image_base_location_resolve(child, uri, policy,
	    &location) == 0);
	assert(location != NULL);
	assert(location->scheme == SCORPI_IMAGE_URI_FILE);
	assert(strcmp(location->original_uri, uri) == 0);
	assert(strcmp(location->resolved_path, expected) == 0);
	scorpi_image_base_location_free(location);
}

int
main(void)
{
	struct scorpi_image_base_location *location;
	struct scorpi_image_uri_policy policy;
	char root[] = "/tmp/scorpi-uri-test-XXXXXX";
	char vm_dir[256], base_dir[256], child[256], expected[256];
	char uri[512];

	assert(mkdtemp(root) != NULL);
	snprintf(vm_dir, sizeof(vm_dir), "%s/vm", root);
	snprintf(base_dir, sizeof(base_dir), "%s/base", root);
	mkdir_or_exists(vm_dir);
	mkdir_or_exists(base_dir);
	snprintf(child, sizeof(child), "%s/vm/active.sco", root);

	snprintf(expected, sizeof(expected), "%s/vm/snap-003.sco", root);
	expect_resolved(child, "file:snap-003.sco", NULL, expected);

	snprintf(expected, sizeof(expected), "%s/base/base.sco", root);
	expect_resolved(child, "file:../base/./base.sco", NULL, expected);

	snprintf(uri, sizeof(uri), "file://%s/base/base.sco", root);
	snprintf(expected, sizeof(expected), "%s/base/base.sco", root);
	expect_resolved(child, uri, NULL, expected);

	location = NULL;
	assert(scorpi_image_base_location_resolve(child,
	    "https://example.invalid/base.sco", NULL, &location) == ENOTSUP);
	assert(location == NULL);

	policy = (struct scorpi_image_uri_policy){
		.allow_absolute_file_uri = false,
		.allowed_root = NULL,
	};
	snprintf(uri, sizeof(uri), "file://%s/base/base.sco", root);
	assert(scorpi_image_base_location_resolve(child, uri, &policy,
	    &location) == EACCES);
	assert(location == NULL);

	policy = (struct scorpi_image_uri_policy){
		.allow_absolute_file_uri = true,
		.allowed_root = vm_dir,
	};
	assert(scorpi_image_base_location_resolve(child, "file:snap-003.sco",
	    &policy, &location) == 0);
	scorpi_image_base_location_free(location);
	location = NULL;
	assert(scorpi_image_base_location_resolve(child, "file:../base/base.sco",
	    &policy, &location) == EACCES);
	assert(location == NULL);

	assert(rmdir(base_dir) == 0);
	assert(rmdir(vm_dir) == 0);
	assert(rmdir(root) == 0);
	return (0);
}
