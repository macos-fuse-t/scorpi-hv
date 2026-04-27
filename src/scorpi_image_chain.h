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
#include "scorpi_image_uri.h"

struct scorpi_image_chain;

struct scorpi_image_chain_open_options {
	bool raw_fallback;
	size_t max_depth;
	struct scorpi_image_uri_policy uri_policy;
	bool has_uri_policy;
};

struct scorpi_image_chain_layer_diagnostic {
	size_t index;
	size_t chain_depth;
	enum scorpi_image_format format;
	const char *format_name;
	bool readonly;
	bool sealed;
	uint64_t virtual_size;
	uint32_t logical_sector_size;
	uint32_t physical_sector_size;
	uint32_t cluster_size;
	bool has_base;
	char *base_uri;
	char *source_uri;
	char *resolved_path;
};

struct scorpi_image_chain_diagnostics {
	size_t layer_count;
	struct scorpi_image_chain_layer_diagnostic *layers;
};

/*
 * Takes ownership of fd on both success and failure.
 */
int	scorpi_image_chain_open_single(const char *path, int fd, bool readonly,
	    const struct scorpi_image_chain_open_options *options,
	    struct scorpi_image_chain **chainp);
int	scorpi_image_chain_open_single_backend(const struct scorpi_image_ops *ops,
	    void *state, struct scorpi_image_chain **chainp);
const struct scorpi_image_info *scorpi_image_chain_top_info(
	    const struct scorpi_image_chain *chain);
size_t	scorpi_image_chain_layer_count(const struct scorpi_image_chain *chain);
const struct scorpi_image_info *scorpi_image_chain_layer_info(
	    const struct scorpi_image_chain *chain, size_t index);
const char *scorpi_image_format_name(enum scorpi_image_format format);
int	scorpi_image_chain_get_diagnostics(
	    const struct scorpi_image_chain *chain,
	    struct scorpi_image_chain_diagnostics *diagnosticsp);
void	scorpi_image_chain_diagnostics_free(
	    struct scorpi_image_chain_diagnostics *diagnostics);
int	scorpi_image_chain_read(struct scorpi_image_chain *chain, void *buf,
	    uint64_t offset, size_t len);
int	scorpi_image_chain_write(struct scorpi_image_chain *chain,
	    const void *buf, uint64_t offset, size_t len);
int	scorpi_image_chain_discard(struct scorpi_image_chain *chain,
	    uint64_t offset, uint64_t length);
int	scorpi_image_chain_flush(struct scorpi_image_chain *chain);
int	scorpi_image_chain_close(struct scorpi_image_chain *chain);

#endif /* _SCORPI_IMAGE_CHAIN_H_ */
