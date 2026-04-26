/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/errno.h>
#include <sys/param.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "scorpi_image_uri.h"

static bool
path_is_absolute(const char *path)
{
	return (path != NULL && path[0] == '/');
}

static int
make_absolute_path(const char *path, char *buf, size_t buflen)
{
	char cwd[MAXPATHLEN];

	if (path == NULL || path[0] == '\0')
		return (EINVAL);
	if (path_is_absolute(path)) {
		if (strlcpy(buf, path, buflen) >= buflen)
			return (ENAMETOOLONG);
		return (0);
	}
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (errno);
	if (snprintf(buf, buflen, "%s/%s", cwd, path) >= (int)buflen)
		return (ENAMETOOLONG);
	return (0);
}

static int
dirname_of_path(const char *path, char *buf, size_t buflen)
{
	const char *slash;
	size_t len;

	slash = strrchr(path, '/');
	if (slash == NULL) {
		if (strlcpy(buf, ".", buflen) >= buflen)
			return (ENAMETOOLONG);
		return (0);
	}
	if (slash == path)
		len = 1;
	else
		len = (size_t)(slash - path);
	if (len + 1 > buflen)
		return (ENAMETOOLONG);
	memcpy(buf, path, len);
	buf[len] = '\0';
	return (0);
}

static int
normalize_absolute_path(const char *path, char **normalizedp)
{
	char *components[MAXPATHLEN / 2];
	char *copy, *token, *cursor, *out;
	size_t count, len, outlen, i;

	if (!path_is_absolute(path))
		return (EINVAL);

	copy = strdup(path);
	if (copy == NULL)
		return (ENOMEM);

	count = 0;
	cursor = copy;
	while ((token = strsep(&cursor, "/")) != NULL) {
		if (token[0] == '\0' || strcmp(token, ".") == 0)
			continue;
		if (strcmp(token, "..") == 0) {
			if (count > 0)
				count--;
			continue;
		}
		if (count == sizeof(components) / sizeof(components[0])) {
			free(copy);
			return (ENAMETOOLONG);
		}
		components[count++] = token;
	}

	if (count == 0) {
		free(copy);
		*normalizedp = strdup("/");
		return (*normalizedp == NULL ? ENOMEM : 0);
	}

	outlen = 1;
	for (i = 0; i < count; i++)
		outlen += strlen(components[i]) + 1;
	out = calloc(1, outlen + 1);
	if (out == NULL) {
		free(copy);
		return (ENOMEM);
	}

	len = 0;
	for (i = 0; i < count; i++) {
		out[len++] = '/';
		(void)strcpy(out + len, components[i]);
		len += strlen(components[i]);
	}

	free(copy);
	*normalizedp = out;
	return (0);
}

static bool
path_is_under_root(const char *path, const char *root)
{
	size_t root_len;

	root_len = strlen(root);
	if (strcmp(root, "/") == 0)
		return (path[0] == '/');
	if (strncmp(path, root, root_len) != 0)
		return (false);
	return (path[root_len] == '\0' || path[root_len] == '/');
}

static int
normalize_policy_root(const char *root, char **normalizedp)
{
	char abs_root[MAXPATHLEN];
	int error;

	error = make_absolute_path(root, abs_root, sizeof(abs_root));
	if (error != 0)
		return (error);
	return (normalize_absolute_path(abs_root, normalizedp));
}

int
scorpi_image_parent_location_resolve(const char *child_path,
    const char *parent_uri, const struct scorpi_image_uri_policy *policy,
    struct scorpi_image_parent_location **locationp)
{
	struct scorpi_image_parent_location *location;
	struct scorpi_image_uri_policy default_policy;
	char abs_child[MAXPATHLEN], base_dir[MAXPATHLEN], joined[MAXPATHLEN];
	char *normalized, *normalized_root;
	const char *file_path, *rest;
	bool absolute_uri;
	int error;

	if (child_path == NULL || parent_uri == NULL || locationp == NULL)
		return (EINVAL);

	default_policy = (struct scorpi_image_uri_policy){
		.allow_absolute_file_uri = true,
		.allowed_root = NULL,
	};
	if (policy == NULL)
		policy = &default_policy;

	if (strncmp(parent_uri, "file:", 5) != 0)
		return (ENOTSUP);

	rest = parent_uri + 5;
	if (rest[0] == '\0')
		return (EINVAL);
	if (strncmp(rest, "///", 3) == 0) {
		absolute_uri = true;
		file_path = rest + 2;
	} else if (rest[0] == '/') {
		return (EINVAL);
	} else {
		absolute_uri = false;
		file_path = rest;
	}

	if (absolute_uri && !policy->allow_absolute_file_uri)
		return (EACCES);

	if (absolute_uri) {
		if (strlcpy(joined, file_path, sizeof(joined)) >= sizeof(joined))
			return (ENAMETOOLONG);
	} else {
		error = make_absolute_path(child_path, abs_child,
		    sizeof(abs_child));
		if (error != 0)
			return (error);
		error = dirname_of_path(abs_child, base_dir, sizeof(base_dir));
		if (error != 0)
			return (error);
		if (snprintf(joined, sizeof(joined), "%s/%s", base_dir,
		    file_path) >= (int)sizeof(joined))
			return (ENAMETOOLONG);
	}

	normalized = NULL;
	error = normalize_absolute_path(joined, &normalized);
	if (error != 0)
		return (error);

	if (policy->allowed_root != NULL) {
		normalized_root = NULL;
		error = normalize_policy_root(policy->allowed_root,
		    &normalized_root);
		if (error != 0) {
			free(normalized);
			return (error);
		}
		if (!path_is_under_root(normalized, normalized_root)) {
			free(normalized_root);
			free(normalized);
			return (EACCES);
		}
		free(normalized_root);
	}

	location = calloc(1, sizeof(*location));
	if (location == NULL) {
		free(normalized);
		return (ENOMEM);
	}
	location->scheme = SCORPI_IMAGE_URI_FILE;
	location->original_uri = strdup(parent_uri);
	if (location->original_uri == NULL) {
		free(location);
		free(normalized);
		return (ENOMEM);
	}
	location->resolved_path = normalized;
	*locationp = location;
	return (0);
}

void
scorpi_image_parent_location_free(struct scorpi_image_parent_location *location)
{
	if (location == NULL)
		return;
	free(location->original_uri);
	free(location->resolved_path);
	free(location);
}
