/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/errno.h>

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "scorpi_image.h"

#define	SCORPI_IMAGE_BACKEND_MAX	32

static pthread_mutex_t scorpi_image_backend_mtx = PTHREAD_MUTEX_INITIALIZER;
static const struct scorpi_image_ops *scorpi_image_backends[
    SCORPI_IMAGE_BACKEND_MAX];

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

int
scorpi_image_backend_register(const struct scorpi_image_ops *ops)
{
	int free_slot, ret;
	size_t i;

	if (!scorpi_image_backend_ops_valid(ops))
		return (EINVAL);

	ret = 0;
	free_slot = -1;
	pthread_mutex_lock(&scorpi_image_backend_mtx);
	for (i = 0; i < SCORPI_IMAGE_BACKEND_MAX; i++) {
		if (scorpi_image_backends[i] == NULL) {
			if (free_slot < 0)
				free_slot = (int)i;
			continue;
		}
		if (strcmp(scorpi_image_backends[i]->name, ops->name) == 0) {
			ret = EEXIST;
			goto out;
		}
	}
	if (free_slot < 0) {
		ret = ENOSPC;
		goto out;
	}
	scorpi_image_backends[free_slot] = ops;
out:
	pthread_mutex_unlock(&scorpi_image_backend_mtx);
	return (ret);
}

int
scorpi_image_backend_unregister(const char *name)
{
	int ret;
	size_t i;

	if (name == NULL || name[0] == '\0')
		return (EINVAL);

	ret = ENOENT;
	pthread_mutex_lock(&scorpi_image_backend_mtx);
	for (i = 0; i < SCORPI_IMAGE_BACKEND_MAX; i++) {
		if (scorpi_image_backends[i] == NULL)
			continue;
		if (strcmp(scorpi_image_backends[i]->name, name) == 0) {
			scorpi_image_backends[i] = NULL;
			ret = 0;
			break;
		}
	}
	pthread_mutex_unlock(&scorpi_image_backend_mtx);
	return (ret);
}

const struct scorpi_image_ops *
scorpi_image_backend_find(const char *name)
{
	const struct scorpi_image_ops *ops;
	size_t i;

	if (name == NULL || name[0] == '\0')
		return (NULL);

	ops = NULL;
	pthread_mutex_lock(&scorpi_image_backend_mtx);
	for (i = 0; i < SCORPI_IMAGE_BACKEND_MAX; i++) {
		if (scorpi_image_backends[i] == NULL)
			continue;
		if (strcmp(scorpi_image_backends[i]->name, name) == 0) {
			ops = scorpi_image_backends[i];
			break;
		}
	}
	pthread_mutex_unlock(&scorpi_image_backend_mtx);
	return (ops);
}

int
scorpi_image_backend_probe(int fd, const struct scorpi_image_ops **ops,
    uint32_t *score)
{
	const struct scorpi_image_ops *best_ops;
	uint32_t best_score, candidate_score;
	int error, ret;
	size_t i;

	if (ops == NULL)
		return (EINVAL);

	best_ops = NULL;
	best_score = 0;
	ret = ENOENT;
	pthread_mutex_lock(&scorpi_image_backend_mtx);
	for (i = 0; i < SCORPI_IMAGE_BACKEND_MAX; i++) {
		if (scorpi_image_backends[i] == NULL)
			continue;
		candidate_score = 0;
		error = scorpi_image_backends[i]->probe(fd, &candidate_score);
		if (error != 0) {
			ret = error;
			goto out;
		}
		if (candidate_score > best_score) {
			best_score = candidate_score;
			best_ops = scorpi_image_backends[i];
			ret = 0;
		}
	}
out:
	pthread_mutex_unlock(&scorpi_image_backend_mtx);
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
