/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/errno.h>

#include <stdint.h>
#include <string.h>

#include "scorpi_image.h"

SET_DECLARE(sco_img_be_set, struct scorpi_image_ops);

static bool
scorpi_image_backend_ops_valid(const struct scorpi_image_ops *ops)
{
	return (ops != NULL && ops->name != NULL && ops->name[0] != '\0' &&
	    ops->probe != NULL && ops->open != NULL &&
	    ops->get_info != NULL && ops->map != NULL &&
	    ops->read != NULL && ops->write != NULL &&
	    ops->discard != NULL && ops->flush != NULL &&
	    ops->close != NULL);
}

const struct scorpi_image_ops *
scorpi_image_backend_find(const char *name)
{
	struct scorpi_image_ops **opsp;

	if (name == NULL || name[0] == '\0')
		return (NULL);

	SET_FOREACH(opsp, sco_img_be_set) {
		if (*opsp == NULL || !scorpi_image_backend_ops_valid(*opsp))
			continue;
		if (strcmp((*opsp)->name, name) == 0)
			return (*opsp);
	}
	return (NULL);
}

int
scorpi_image_backend_probe(int fd, const struct scorpi_image_ops **ops,
    uint32_t *score)
{
	const struct scorpi_image_ops *best_ops;
	struct scorpi_image_ops **opsp;
	uint32_t best_score, candidate_score;
	int error, ret;

	if (ops == NULL)
		return (EINVAL);

	best_ops = NULL;
	best_score = 0;
	ret = ENOENT;
	SET_FOREACH(opsp, sco_img_be_set) {
		if (*opsp == NULL || !scorpi_image_backend_ops_valid(*opsp))
			continue;
		candidate_score = 0;
		error = (*opsp)->probe(fd, &candidate_score);
		if (error != 0)
			return (error);
		if (candidate_score > best_score) {
			best_score = candidate_score;
			best_ops = *opsp;
			ret = 0;
		}
	}
	if (ret == 0) {
		*ops = best_ops;
		if (score != NULL)
			*score = best_score;
	} else {
		*ops = NULL;
		if (score != NULL)
			*score = 0;
	}
	return (ret);
}
