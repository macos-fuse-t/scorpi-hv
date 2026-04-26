/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/errno.h>

#include <stdbool.h>
#include <stdlib.h>

#include "scorpi_image_chain.h"
#include "scorpi_image_raw.h"

struct scorpi_image_layer {
	const struct scorpi_image_ops *ops;
	void *state;
	struct scorpi_image_info info;
};

struct scorpi_image_chain {
	size_t layer_count;
	struct scorpi_image_layer *layers;
};

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
	chain->layers = calloc(1, sizeof(chain->layers[0]));
	if (chain->layers == NULL) {
		free(chain);
		return (ENOMEM);
	}

	chain->layer_count = 1;
	chain->layers[0].ops = ops;
	chain->layers[0].state = state;
	error = ops->get_info(state, &chain->layers[0].info);
	if (error != 0) {
		free(chain->layers);
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
	uint32_t score;
	void *state;
	int error;

	if (path == NULL || fd < 0 || chainp == NULL)
		return (EINVAL);

	default_options = (struct scorpi_image_chain_open_options){
		.raw_fallback = false,
	};
	if (options == NULL)
		options = &default_options;

	ops = NULL;
	score = 0;
	error = scorpi_image_backend_probe(fd, &ops, &score);
	if (error != 0 && error != ENOENT)
		return (error);
	if ((error == ENOENT || score == 0) && options->raw_fallback)
		ops = scorpi_image_raw_backend();
	if (ops == NULL)
		return (ENOENT);

	state = NULL;
	error = ops->open(path, fd, readonly, &state);
	if (error != 0)
		return (error);
	error = scorpi_image_chain_open_single_backend(ops, state, chainp);
	if (error != 0)
		ops->close(state);
	return (error);
}

const struct scorpi_image_info *
scorpi_image_chain_top_info(const struct scorpi_image_chain *chain)
{
	if (chain == NULL || chain->layer_count == 0)
		return (NULL);
	return (&chain->layers[0].info);
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
		if (chain->layers[i].ops == NULL)
			continue;
		error = chain->layers[i].ops->close(chain->layers[i].state);
		if (first_error == 0 && error != 0)
			first_error = error;
	}
	free(chain->layers);
	free(chain);
	return (first_error);
}
