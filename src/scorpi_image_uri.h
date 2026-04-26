/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#ifndef _SCORPI_IMAGE_URI_H_
#define _SCORPI_IMAGE_URI_H_

#include <stdbool.h>

enum scorpi_image_uri_scheme {
	SCORPI_IMAGE_URI_FILE = 1,
};

struct scorpi_image_uri_policy {
	bool allow_absolute_file_uri;
	const char *allowed_root;
};

struct scorpi_image_parent_location {
	enum scorpi_image_uri_scheme scheme;
	char *original_uri;
	char *resolved_path;
};

int	scorpi_image_parent_location_resolve(const char *child_path,
	    const char *parent_uri, const struct scorpi_image_uri_policy *policy,
	    struct scorpi_image_parent_location **locationp);
void	scorpi_image_parent_location_free(
	    struct scorpi_image_parent_location *location);

#endif /* _SCORPI_IMAGE_URI_H_ */
