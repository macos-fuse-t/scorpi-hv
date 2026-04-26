/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#ifndef _SCORPI_IMAGE_CHAIN_H_
#define _SCORPI_IMAGE_CHAIN_H_

#include <stddef.h>
#include <stdint.h>

#include "scorpi_image.h"

struct scorpi_image_chain;

struct scorpi_image_chain_open_options {
	bool raw_fallback;
};

int	scorpi_image_chain_open_single(const char *path, int fd, bool readonly,
	    const struct scorpi_image_chain_open_options *options,
	    struct scorpi_image_chain **chainp);
int	scorpi_image_chain_open_single_backend(const struct scorpi_image_ops *ops,
	    void *state, struct scorpi_image_chain **chainp);
const struct scorpi_image_info *scorpi_image_chain_top_info(
	    const struct scorpi_image_chain *chain);
int	scorpi_image_chain_read(struct scorpi_image_chain *chain, void *buf,
	    uint64_t offset, size_t len);
int	scorpi_image_chain_write(struct scorpi_image_chain *chain,
	    const void *buf, uint64_t offset, size_t len);
int	scorpi_image_chain_discard(struct scorpi_image_chain *chain,
	    uint64_t offset, uint64_t length);
int	scorpi_image_chain_flush(struct scorpi_image_chain *chain);
int	scorpi_image_chain_close(struct scorpi_image_chain *chain);

#endif /* _SCORPI_IMAGE_CHAIN_H_ */
