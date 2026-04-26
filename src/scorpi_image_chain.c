/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "scorpi_image_chain.h"
#include "scorpi_image_raw.h"

struct scorpi_image_layer {
	const struct scorpi_image_ops *ops;
	void *state;
	struct scorpi_image_info info;
	char *path;
	dev_t dev;
	ino_t ino;
	bool has_file_id;
};

struct scorpi_image_chain {
	size_t layer_count;
	struct scorpi_image_layer *layers;
};

static int
scorpi_image_chain_add_layer(struct scorpi_image_chain *chain,
    const struct scorpi_image_ops *ops, void *state, const char *path,
    const struct stat *sb)
{
	struct scorpi_image_layer *layers, *layer;
	int error;

	layers = realloc(chain->layers,
	    (chain->layer_count + 1) * sizeof(chain->layers[0]));
	if (layers == NULL)
		return (ENOMEM);
	chain->layers = layers;
	layer = &chain->layers[chain->layer_count];
	memset(layer, 0, sizeof(*layer));
	layer->ops = ops;
	layer->state = state;
	if (path != NULL) {
		layer->path = strdup(path);
		if (layer->path == NULL)
			return (ENOMEM);
	}
	if (sb != NULL) {
		layer->dev = sb->st_dev;
		layer->ino = sb->st_ino;
		layer->has_file_id = true;
	}
	error = ops->get_info(state, &layer->info);
	if (error != 0) {
		free(layer->path);
		memset(layer, 0, sizeof(*layer));
		return (error);
	}
	chain->layer_count++;
	return (0);
}

static bool
scorpi_image_chain_has_file_id(const struct scorpi_image_chain *chain,
    const struct stat *sb)
{
	size_t i;

	for (i = 0; i < chain->layer_count; i++) {
		if (!chain->layers[i].has_file_id)
			continue;
		if (chain->layers[i].dev == sb->st_dev &&
		    chain->layers[i].ino == sb->st_ino)
			return (true);
	}
	return (false);
}

static int
scorpi_image_open_layer(const char *path, int fd, bool readonly,
    bool raw_fallback, const struct scorpi_image_ops **opsp, void **statep,
    struct stat *sbp)
{
	const struct scorpi_image_ops *ops;
	uint32_t score;
	int error;

	if (fstat(fd, sbp) != 0) {
		error = errno;
		close(fd);
		return (error);
	}

	ops = NULL;
	score = 0;
	error = scorpi_image_backend_probe(fd, &ops, &score);
	if (error != 0 && error != ENOENT) {
		close(fd);
		return (error);
	}
	if ((error == ENOENT || score == 0) && raw_fallback)
		ops = scorpi_image_raw_backend();
	if (ops == NULL) {
		close(fd);
		return (ENOENT);
	}

	*statep = NULL;
	error = ops->open(path, fd, readonly, statep);
	if (error != 0) {
		close(fd);
		return (error);
	}
	*opsp = ops;
	return (0);
}

static size_t
scorpi_image_chain_max_depth(
    const struct scorpi_image_chain_open_options *options)
{
	if (options->max_depth == 0)
		return (32);
	return (options->max_depth);
}

int
scorpi_image_chain_open_single_backend(const struct scorpi_image_ops *ops,
    void *state, struct scorpi_image_chain **chainp)
{
	struct scorpi_image_chain *chain;
	int error;

	if (ops == NULL || state == NULL || chainp == NULL)
		return (EINVAL);

	chain = calloc(1, sizeof(*chain));
	if (chain == NULL)
		return (ENOMEM);
	error = scorpi_image_chain_add_layer(chain, ops, state, NULL, NULL);
	if (error != 0) {
		ops->close(state);
		free(chain);
		return (error);
	}

	*chainp = chain;
	return (0);
}

int
scorpi_image_chain_open_single(const char *path, int fd, bool readonly,
    const struct scorpi_image_chain_open_options *options,
    struct scorpi_image_chain **chainp)
{
	const struct scorpi_image_ops *ops;
	struct scorpi_image_chain_open_options default_options;
	struct scorpi_image_parent_location *location;
	struct scorpi_image_chain *chain;
	struct stat sb;
	const char *current_path;
	char *owned_path;
	void *state;
	size_t max_depth;
	int error;

	if (path == NULL || fd < 0 || chainp == NULL)
		return (EINVAL);
	*chainp = NULL;

	default_options = (struct scorpi_image_chain_open_options){
		.raw_fallback = false,
	};
	if (options == NULL)
		options = &default_options;

	chain = calloc(1, sizeof(*chain));
	if (chain == NULL) {
		close(fd);
		return (ENOMEM);
	}

	max_depth = scorpi_image_chain_max_depth(options);
	current_path = path;
	owned_path = NULL;
	for (;;) {
		ops = NULL;
		state = NULL;
		error = scorpi_image_open_layer(current_path, fd, readonly,
		    options->raw_fallback, &ops, &state, &sb);
		fd = -1;
		if (error != 0)
			goto err;
		if (scorpi_image_chain_has_file_id(chain, &sb)) {
			ops->close(state);
			error = ELOOP;
			goto err;
		}
		error = scorpi_image_chain_add_layer(chain, ops, state,
		    current_path, &sb);
		if (error != 0) {
			ops->close(state);
			goto err;
		}
		free(owned_path);
		owned_path = NULL;

		if (!chain->layers[chain->layer_count - 1].info.has_parent)
			break;
		if (chain->layer_count >= max_depth) {
			error = E2BIG;
			goto err;
		}

		location = NULL;
		error = scorpi_image_parent_location_resolve(
		    chain->layers[chain->layer_count - 1].path,
		    chain->layers[chain->layer_count - 1].info.parent_uri,
		    options->has_uri_policy ? &options->uri_policy : NULL,
		    &location);
		if (error != 0)
			goto err;
		owned_path = strdup(location->resolved_path);
		scorpi_image_parent_location_free(location);
		if (owned_path == NULL) {
			error = ENOMEM;
			goto err;
		}
		fd = open(owned_path, O_RDONLY);
		if (fd < 0) {
			error = errno;
			goto err;
		}
		current_path = owned_path;
		readonly = true;
	}

	free(owned_path);
	*chainp = chain;
	return (0);
err:
	free(owned_path);
	if (fd >= 0)
		close(fd);
	scorpi_image_chain_close(chain);
	return (error);
}

const struct scorpi_image_info *
scorpi_image_chain_top_info(const struct scorpi_image_chain *chain)
{
	if (chain == NULL || chain->layer_count == 0)
		return (NULL);
	return (&chain->layers[0].info);
}

size_t
scorpi_image_chain_layer_count(const struct scorpi_image_chain *chain)
{
	if (chain == NULL)
		return (0);
	return (chain->layer_count);
}

const struct scorpi_image_info *
scorpi_image_chain_layer_info(const struct scorpi_image_chain *chain,
    size_t index)
{
	if (chain == NULL || index >= chain->layer_count)
		return (NULL);
	return (&chain->layers[index].info);
}

int
scorpi_image_chain_read(struct scorpi_image_chain *chain, void *buf,
    uint64_t offset, size_t len)
{
	if (chain == NULL || chain->layer_count == 0)
		return (EINVAL);
	return (chain->layers[0].ops->read(chain->layers[0].state, buf,
	    offset, len));
}

int
scorpi_image_chain_write(struct scorpi_image_chain *chain, const void *buf,
    uint64_t offset, size_t len)
{
	if (chain == NULL || chain->layer_count == 0)
		return (EINVAL);
	if (chain->layers[0].info.readonly || chain->layers[0].info.sealed)
		return (EROFS);
	return (chain->layers[0].ops->write(chain->layers[0].state, buf,
	    offset, len));
}

int
scorpi_image_chain_discard(struct scorpi_image_chain *chain, uint64_t offset,
    uint64_t length)
{
	if (chain == NULL || chain->layer_count == 0)
		return (EINVAL);
	if (chain->layers[0].info.readonly || chain->layers[0].info.sealed)
		return (EROFS);
	return (chain->layers[0].ops->discard(chain->layers[0].state, offset,
	    length));
}

int
scorpi_image_chain_flush(struct scorpi_image_chain *chain)
{
	if (chain == NULL || chain->layer_count == 0)
		return (EINVAL);
	return (chain->layers[0].ops->flush(chain->layers[0].state));
}

int
scorpi_image_chain_close(struct scorpi_image_chain *chain)
{
	int error, first_error;
	size_t i;

	if (chain == NULL)
		return (0);

	first_error = 0;
	for (i = 0; i < chain->layer_count; i++) {
		error = 0;
		if (chain->layers[i].ops != NULL)
			error = chain->layers[i].ops->close(
			    chain->layers[i].state);
		if (first_error == 0 && error != 0)
			first_error = error;
		free(chain->layers[i].path);
		free(chain->layers[i].info.parent_uri);
	}
	free(chain->layers);
	free(chain);
	return (first_error);
}
