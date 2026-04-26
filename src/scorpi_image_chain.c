/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "scorpi_image_chain.h"
#include "scorpi_image_raw.h"

struct scorpi_image_read_cache {
	bool valid;
	uint64_t offset;
	uint64_t length;
	enum scorpi_image_extent_state state;
	size_t image_index;
};

struct scorpi_image_chain {
	pthread_rwlock_t lock;
	pthread_mutex_t cache_mtx;
	size_t image_count;
	struct scorpi_image **images;
	struct scorpi_image_read_cache read_cache;
};

static int
scorpi_image_chain_alloc(struct scorpi_image_chain **chainp)
{
	struct scorpi_image_chain *chain;
	int error;

	if (chainp == NULL)
		return (EINVAL);
	chain = calloc(1, sizeof(*chain));
	if (chain == NULL)
		return (ENOMEM);
	error = pthread_rwlock_init(&chain->lock, NULL);
	if (error != 0) {
		free(chain);
		return (error);
	}
	error = pthread_mutex_init(&chain->cache_mtx, NULL);
	if (error != 0) {
		pthread_rwlock_destroy(&chain->lock);
		free(chain);
		return (error);
	}
	*chainp = chain;
	return (0);
}

static void
scorpi_image_chain_read_cache_invalidate(struct scorpi_image_chain *chain)
{
	if (chain == NULL)
		return;
	pthread_mutex_lock(&chain->cache_mtx);
	chain->read_cache.valid = false;
	pthread_mutex_unlock(&chain->cache_mtx);
}

static int
scorpi_image_close_image(struct scorpi_image *image)
{
	int error;

	if (image == NULL)
		return (0);
	error = 0;
	if (image->ops != NULL)
		error = image->ops->close(image->state);
	free(image->path);
	free(image->info.base_uri);
	free(image);
	return (error);
}

static int
scorpi_image_alloc(const struct scorpi_image_ops *ops, void *state,
    const char *path, const struct stat *sb, struct scorpi_image **imagep)
{
	struct scorpi_image *image;
	int error;

	image = calloc(1, sizeof(*image));
	if (image == NULL)
		return (ENOMEM);
	image->ops = ops;
	image->state = state;
	if (path != NULL) {
		image->path = strdup(path);
		if (image->path == NULL) {
			free(image);
			return (ENOMEM);
		}
	}
	if (sb != NULL) {
		image->dev = sb->st_dev;
		image->ino = sb->st_ino;
		image->has_file_id = true;
	}
	error = ops->get_info(state, &image->info);
	if (error != 0) {
		free(image->path);
		free(image);
		return (error);
	}
	*imagep = image;
	return (0);
}

static int
scorpi_image_chain_add_image(struct scorpi_image_chain *chain,
    struct scorpi_image *image)
{
	struct scorpi_image **images;

	images = realloc(chain->images,
	    (chain->image_count + 1) * sizeof(chain->images[0]));
	if (images == NULL)
		return (ENOMEM);
	chain->images = images;
	if (chain->image_count > 0)
		chain->images[chain->image_count - 1]->base = image;
	chain->images[chain->image_count++] = image;
	return (0);
}

static bool
scorpi_image_chain_has_file_id(const struct scorpi_image_chain *chain,
    const struct stat *sb)
{
	size_t i;

	for (i = 0; i < chain->image_count; i++) {
		if (!chain->images[i]->has_file_id)
			continue;
		if (chain->images[i]->dev == sb->st_dev &&
		    chain->images[i]->ino == sb->st_ino)
			return (true);
	}
	return (false);
}

static int
scorpi_image_open_one(const char *path, int fd, bool readonly,
    bool raw_fallback, struct scorpi_image **imagep, struct stat *sbp)
{
	const struct scorpi_image_ops *ops;
	struct scorpi_image *image;
	void *state;
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

	state = NULL;
	error = ops->open(path, fd, readonly, &state);
	if (error != 0) {
		close(fd);
		return (error);
	}
	image = NULL;
	error = scorpi_image_alloc(ops, state, path, sbp, &image);
	if (error != 0) {
		ops->close(state);
		return (error);
	}
	*imagep = image;
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

static int
scorpi_image_chain_validate(const struct scorpi_image_chain *chain,
    bool require_writable_top)
{
	const struct scorpi_image_info *top_info, *info;
	size_t writable_count, i;

	if (chain == NULL || chain->image_count == 0)
		return (EINVAL);

	top_info = &chain->images[0]->info;
	if (top_info->virtual_size == 0 || top_info->logical_sector_size == 0)
		return (EINVAL);
	if (require_writable_top && (top_info->readonly || top_info->sealed))
		return (EROFS);

	writable_count = 0;
	for (i = 0; i < chain->image_count; i++) {
		info = &chain->images[i]->info;
		if (info->virtual_size != top_info->virtual_size)
			return (EINVAL);
		if (info->logical_sector_size != top_info->logical_sector_size)
			return (EINVAL);
		if (!info->readonly && !info->sealed)
			writable_count++;
		if (i > 0 && !info->readonly && !info->sealed)
			return (EROFS);
		if (i + 1 < chain->image_count &&
		    info->has_base_uuid) {
			if (!chain->images[i + 1]->info.has_image_uuid)
				return (EINVAL);
			if (memcmp(info->base_uuid,
			    chain->images[i + 1]->info.image_uuid,
			    sizeof(info->base_uuid)) != 0)
				return (EINVAL);
		}
		if (i + 1 < chain->image_count &&
		    info->has_base_digest) {
			if (!chain->images[i + 1]->info.has_image_digest)
				return (EINVAL);
			if (memcmp(info->base_digest,
			    chain->images[i + 1]->info.image_digest,
			    sizeof(info->base_digest)) != 0)
				return (EINVAL);
		}
	}
	if (writable_count > 1)
		return (EROFS);
	return (0);
}

static int
scorpi_image_extent_cover(const struct scorpi_image_extent *extent,
    uint64_t offset, uint64_t max_length, uint64_t *lengthp)
{
	uint64_t extent_end, length;

	if (extent == NULL || lengthp == NULL || max_length == 0)
		return (EINVAL);
	if (extent->length == 0 || extent->offset > offset)
		return (EINVAL);
	if (extent->length > UINT64_MAX - extent->offset)
		return (EINVAL);
	extent_end = extent->offset + extent->length;
	if (extent_end <= offset)
		return (EINVAL);
	length = extent_end - offset;
	if (length > max_length)
		length = max_length;
	if (length == 0)
		return (EINVAL);
	*lengthp = length;
	return (0);
}

static int
scorpi_image_chain_cache_read(struct scorpi_image_chain *chain, void *buf,
    uint64_t offset, size_t len, size_t *readp)
{
	struct scorpi_image_read_cache cache;
	uint64_t cache_end, length;
	struct scorpi_image *image;
	int error;

	pthread_mutex_lock(&chain->cache_mtx);
	cache = chain->read_cache;
	pthread_mutex_unlock(&chain->cache_mtx);

	if (!cache.valid)
		return (ENOENT);
	if (offset < cache.offset)
		return (ENOENT);
	if (cache.length > UINT64_MAX - cache.offset)
		return (ENOENT);
	cache_end = cache.offset + cache.length;
	if (offset >= cache_end)
		return (ENOENT);
	length = cache_end - offset;
	if (length > len)
		length = len;
	if (length > SIZE_MAX)
		return (EINVAL);

	switch (cache.state) {
	case SCORPI_IMAGE_EXTENT_PRESENT:
		if (cache.image_index >= chain->image_count)
			return (ENOENT);
		image = chain->images[cache.image_index];
		error = image->ops->read(image->state, buf, offset,
		    (size_t)length);
		if (error != 0) {
			scorpi_image_chain_read_cache_invalidate(chain);
			return (error);
		}
		break;
	case SCORPI_IMAGE_EXTENT_ZERO:
	case SCORPI_IMAGE_EXTENT_DISCARDED:
	case SCORPI_IMAGE_EXTENT_ABSENT:
		memset(buf, 0, (size_t)length);
		break;
	default:
		return (EINVAL);
	}

	*readp = (size_t)length;
	return (0);
}

static int
scorpi_image_chain_resolve_read(struct scorpi_image_chain *chain, void *buf,
    uint64_t offset, size_t len, size_t *readp)
{
	struct scorpi_image_extent extent;
	struct scorpi_image *image;
	uint64_t limit, covered;
	int error;
	size_t i;

	limit = len;
	for (i = 0; i < chain->image_count; i++) {
		image = chain->images[i];
		memset(&extent, 0, sizeof(extent));
		error = image->ops->map(image->state, offset, (size_t)limit,
		    &extent);
		if (error != 0)
			return (error);
		error = scorpi_image_extent_cover(&extent, offset, limit,
		    &covered);
		if (error != 0)
			return (error);

		switch (extent.state) {
		case SCORPI_IMAGE_EXTENT_PRESENT:
			error = image->ops->read(image->state, buf, offset,
			    (size_t)covered);
			if (error != 0)
				return (error);
			pthread_mutex_lock(&chain->cache_mtx);
			chain->read_cache = (struct scorpi_image_read_cache){
				.valid = true,
				.offset = offset,
				.length = covered,
				.state = SCORPI_IMAGE_EXTENT_PRESENT,
				.image_index = i,
			};
			pthread_mutex_unlock(&chain->cache_mtx);
			*readp = (size_t)covered;
			return (0);
		case SCORPI_IMAGE_EXTENT_ZERO:
		case SCORPI_IMAGE_EXTENT_DISCARDED:
			memset(buf, 0, (size_t)covered);
			pthread_mutex_lock(&chain->cache_mtx);
			chain->read_cache = (struct scorpi_image_read_cache){
				.valid = true,
				.offset = offset,
				.length = covered,
				.state = extent.state,
			};
			pthread_mutex_unlock(&chain->cache_mtx);
			*readp = (size_t)covered;
			return (0);
		case SCORPI_IMAGE_EXTENT_ABSENT:
			limit = covered;
			break;
		default:
			return (EINVAL);
		}
	}

	memset(buf, 0, (size_t)limit);
	pthread_mutex_lock(&chain->cache_mtx);
	chain->read_cache = (struct scorpi_image_read_cache){
		.valid = true,
		.offset = offset,
		.length = limit,
		.state = SCORPI_IMAGE_EXTENT_ABSENT,
	};
	pthread_mutex_unlock(&chain->cache_mtx);
	*readp = (size_t)limit;
	return (0);
}

static int
scorpi_image_chain_read_nolock(struct scorpi_image_chain *chain, void *buf,
    uint64_t offset, size_t len)
{
	uint8_t *p;
	uint64_t end;
	size_t n;
	int error;

	if (chain == NULL || chain->image_count == 0)
		return (EINVAL);
	if (buf == NULL && len != 0)
		return (EINVAL);
	if (len == 0)
		return (0);
	if (offset > chain->images[0]->info.virtual_size ||
	    (uint64_t)len > chain->images[0]->info.virtual_size - offset ||
	    offset > UINT64_MAX - (uint64_t)len)
		return (EINVAL);

	end = offset + (uint64_t)len;
	p = buf;
	while (offset < end) {
		n = 0;
		error = scorpi_image_chain_cache_read(chain, p, offset,
		    (size_t)(end - offset), &n);
		if (error == ENOENT)
			error = scorpi_image_chain_resolve_read(chain, p, offset,
			    (size_t)(end - offset), &n);
		if (error != 0)
			return (error);
		if (n == 0)
			return (EIO);
		offset += n;
		p += n;
	}
	return (0);
}

static int
scorpi_image_chain_write_nolock(struct scorpi_image_chain *chain,
    const void *buf, uint64_t offset, size_t len)
{
	const struct scorpi_image_info *top_info;
	struct scorpi_image *top;
	const uint8_t *p;
	uint8_t *cluster;
	uint64_t end, cluster_size, cluster_start, cluster_len;
	uint64_t write_start, write_end, chunk_len;
	int error;

	if (chain == NULL || chain->image_count == 0)
		return (EINVAL);
	if (buf == NULL && len != 0)
		return (EINVAL);
	if (len == 0)
		return (0);
	top = chain->images[0];
	top_info = &top->info;
	if (offset > top_info->virtual_size ||
	    (uint64_t)len > top_info->virtual_size - offset ||
	    offset > UINT64_MAX - (uint64_t)len)
		return (EINVAL);
	if (top_info->readonly || top_info->sealed)
		return (EROFS);

	cluster_size = top_info->cluster_size;
	if (cluster_size == 0 || top->ops->name == NULL ||
	    strcmp(top->ops->name, "sco") != 0)
		return (top->ops->write(top->state, buf, offset, len));

	end = offset + (uint64_t)len;
	p = buf;
	while (offset < end) {
		cluster_start = offset - (offset % cluster_size);
		cluster_len = top_info->virtual_size - cluster_start;
		if (cluster_len > cluster_size)
			cluster_len = cluster_size;
		write_start = offset;
		write_end = cluster_start + cluster_len;
		if (write_end > end)
			write_end = end;
		chunk_len = write_end - write_start;
		if (chunk_len > SIZE_MAX || cluster_len > SIZE_MAX)
			return (EINVAL);

		if (write_start == cluster_start && chunk_len == cluster_len) {
			error = top->ops->write(top->state, p, cluster_start,
			    (size_t)cluster_len);
			if (error != 0)
				return (error);
		} else {
			cluster = malloc((size_t)cluster_len);
			if (cluster == NULL)
				return (ENOMEM);
			error = scorpi_image_chain_read_nolock(chain, cluster,
			    cluster_start, (size_t)cluster_len);
			if (error == 0) {
				memcpy(cluster + (write_start - cluster_start),
				    p, (size_t)chunk_len);
				error = top->ops->write(top->state, cluster,
				    cluster_start, (size_t)cluster_len);
			}
			free(cluster);
			if (error != 0)
				return (error);
		}
		offset += chunk_len;
		p += chunk_len;
	}
	return (0);
}

static int
scorpi_image_chain_discard_nolock(struct scorpi_image_chain *chain,
    uint64_t offset, uint64_t length)
{
	const struct scorpi_image_info *top_info;
	struct scorpi_image *top;
	uint8_t *cluster;
	uint64_t end, cluster_size, cluster_start, cluster_len;
	uint64_t discard_start, discard_end, chunk_len;
	int error;

	if (chain == NULL || chain->image_count == 0)
		return (EINVAL);
	if (length == 0)
		return (0);
	top = chain->images[0];
	top_info = &top->info;
	if (offset > top_info->virtual_size ||
	    length > top_info->virtual_size - offset ||
	    offset > UINT64_MAX - length)
		return (EINVAL);
	if (top_info->readonly || top_info->sealed)
		return (EROFS);

	cluster_size = top_info->cluster_size;
	if (cluster_size == 0 || top->ops->name == NULL ||
	    strcmp(top->ops->name, "sco") != 0)
		return (top->ops->discard(top->state, offset, length));

	end = offset + length;
	while (offset < end) {
		cluster_start = offset - (offset % cluster_size);
		cluster_len = top_info->virtual_size - cluster_start;
		if (cluster_len > cluster_size)
			cluster_len = cluster_size;
		discard_start = offset;
		discard_end = cluster_start + cluster_len;
		if (discard_end > end)
			discard_end = end;
		chunk_len = discard_end - discard_start;
		if (chunk_len > SIZE_MAX || cluster_len > SIZE_MAX)
			return (EINVAL);

		if (discard_start == cluster_start && chunk_len == cluster_len) {
			error = top->ops->discard(top->state, cluster_start,
			    cluster_len);
			if (error != 0)
				return (error);
		} else {
			cluster = malloc((size_t)cluster_len);
			if (cluster == NULL)
				return (ENOMEM);
			error = scorpi_image_chain_read_nolock(chain, cluster,
			    cluster_start, (size_t)cluster_len);
			if (error == 0) {
				memset(cluster + (discard_start - cluster_start),
				    0, (size_t)chunk_len);
				error = top->ops->write(top->state, cluster,
				    cluster_start, (size_t)cluster_len);
			}
			free(cluster);
			if (error != 0)
				return (error);
		}
		offset += chunk_len;
	}
	return (0);
}

int
scorpi_image_chain_open_single_backend(const struct scorpi_image_ops *ops,
    void *state, struct scorpi_image_chain **chainp)
{
	struct scorpi_image_chain *chain;
	struct scorpi_image *image;
	int error;

	if (ops == NULL || state == NULL || chainp == NULL)
		return (EINVAL);
	*chainp = NULL;

	error = scorpi_image_chain_alloc(&chain);
	if (error != 0) {
		ops->close(state);
		return (error);
	}
	image = NULL;
	error = scorpi_image_alloc(ops, state, NULL, NULL, &image);
	if (error != 0) {
		ops->close(state);
		scorpi_image_chain_close(chain);
		return (error);
	}
	error = scorpi_image_chain_add_image(chain, image);
	if (error != 0) {
		scorpi_image_close_image(image);
		scorpi_image_chain_close(chain);
		return (error);
	}
	error = scorpi_image_chain_validate(chain, false);
	if (error != 0) {
		scorpi_image_chain_close(chain);
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
	struct scorpi_image_chain_open_options default_options;
	struct scorpi_image_base_location *location;
	struct scorpi_image_chain *chain;
	struct scorpi_image *image;
	struct stat sb;
	const char *current_path;
	char *owned_path;
	size_t max_depth;
	bool require_writable_top;
	int error;

	if (path == NULL || fd < 0 || chainp == NULL)
		return (EINVAL);
	*chainp = NULL;

	default_options = (struct scorpi_image_chain_open_options){
		.raw_fallback = false,
	};
	if (options == NULL)
		options = &default_options;

	error = scorpi_image_chain_alloc(&chain);
	if (error != 0) {
		close(fd);
		return (error);
	}

	max_depth = scorpi_image_chain_max_depth(options);
	require_writable_top = !readonly;
	current_path = path;
	owned_path = NULL;
	for (;;) {
		image = NULL;
		error = scorpi_image_open_one(current_path, fd, readonly,
		    options->raw_fallback, &image, &sb);
		fd = -1;
		if (error != 0)
			goto err;
		if (scorpi_image_chain_has_file_id(chain, &sb)) {
			scorpi_image_close_image(image);
			error = ELOOP;
			goto err;
		}
		error = scorpi_image_chain_add_image(chain, image);
		if (error != 0) {
			scorpi_image_close_image(image);
			goto err;
		}
		free(owned_path);
		owned_path = NULL;

		image = chain->images[chain->image_count - 1];
		if (!image->info.has_base)
			break;
		if (chain->image_count >= max_depth) {
			error = E2BIG;
			goto err;
		}

		location = NULL;
		error = scorpi_image_base_location_resolve(image->path,
		    image->info.base_uri,
		    options->has_uri_policy ? &options->uri_policy : NULL,
		    &location);
		if (error != 0)
			goto err;
		owned_path = strdup(location->resolved_path);
		scorpi_image_base_location_free(location);
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

	error = scorpi_image_chain_validate(chain, require_writable_top);
	if (error != 0)
		goto err;

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
	struct scorpi_image_chain *mutable_chain;
	const struct scorpi_image_info *info;

	if (chain == NULL)
		return (NULL);
	mutable_chain = (struct scorpi_image_chain *)chain;
	if (pthread_rwlock_rdlock(&mutable_chain->lock) != 0)
		return (NULL);
	info = chain->image_count == 0 ? NULL : &chain->images[0]->info;
	pthread_rwlock_unlock(&mutable_chain->lock);
	return (info);
}

size_t
scorpi_image_chain_layer_count(const struct scorpi_image_chain *chain)
{
	struct scorpi_image_chain *mutable_chain;
	size_t count;

	if (chain == NULL)
		return (0);
	mutable_chain = (struct scorpi_image_chain *)chain;
	if (pthread_rwlock_rdlock(&mutable_chain->lock) != 0)
		return (0);
	count = chain->image_count;
	pthread_rwlock_unlock(&mutable_chain->lock);
	return (count);
}

const struct scorpi_image_info *
scorpi_image_chain_layer_info(const struct scorpi_image_chain *chain,
    size_t index)
{
	struct scorpi_image_chain *mutable_chain;
	const struct scorpi_image_info *info;

	if (chain == NULL)
		return (NULL);
	mutable_chain = (struct scorpi_image_chain *)chain;
	if (pthread_rwlock_rdlock(&mutable_chain->lock) != 0)
		return (NULL);
	info = index >= chain->image_count ? NULL : &chain->images[index]->info;
	pthread_rwlock_unlock(&mutable_chain->lock);
	return (info);
}

int
scorpi_image_chain_read(struct scorpi_image_chain *chain, void *buf,
    uint64_t offset, size_t len)
{
	int error;

	if (chain == NULL)
		return (EINVAL);
	error = pthread_rwlock_rdlock(&chain->lock);
	if (error != 0)
		return (error);
	error = scorpi_image_chain_read_nolock(chain, buf, offset, len);
	pthread_rwlock_unlock(&chain->lock);
	return (error);
}

int
scorpi_image_chain_write(struct scorpi_image_chain *chain, const void *buf,
    uint64_t offset, size_t len)
{
	int error;

	if (chain == NULL)
		return (EINVAL);
	error = pthread_rwlock_wrlock(&chain->lock);
	if (error != 0)
		return (error);
	if (chain->image_count == 0) {
		error = EINVAL;
		goto out;
	}
	if (chain->images[0]->info.readonly || chain->images[0]->info.sealed) {
		error = EROFS;
		goto out;
	}
	scorpi_image_chain_read_cache_invalidate(chain);
	error = scorpi_image_chain_write_nolock(chain, buf, offset, len);
	scorpi_image_chain_read_cache_invalidate(chain);
out:
	pthread_rwlock_unlock(&chain->lock);
	return (error);
}

int
scorpi_image_chain_discard(struct scorpi_image_chain *chain, uint64_t offset,
    uint64_t length)
{
	int error;

	if (chain == NULL)
		return (EINVAL);
	error = pthread_rwlock_wrlock(&chain->lock);
	if (error != 0)
		return (error);
	if (chain->image_count == 0) {
		error = EINVAL;
		goto out;
	}
	if (chain->images[0]->info.readonly || chain->images[0]->info.sealed) {
		error = EROFS;
		goto out;
	}
	scorpi_image_chain_read_cache_invalidate(chain);
	error = scorpi_image_chain_discard_nolock(chain, offset, length);
	scorpi_image_chain_read_cache_invalidate(chain);
out:
	pthread_rwlock_unlock(&chain->lock);
	return (error);
}

int
scorpi_image_chain_flush(struct scorpi_image_chain *chain)
{
	int error;

	if (chain == NULL)
		return (EINVAL);
	error = pthread_rwlock_wrlock(&chain->lock);
	if (error != 0)
		return (error);
	if (chain->image_count == 0)
		error = EINVAL;
	else
		error = chain->images[0]->ops->flush(chain->images[0]->state);
	pthread_rwlock_unlock(&chain->lock);
	return (error);
}

int
scorpi_image_chain_close(struct scorpi_image_chain *chain)
{
	int error, first_error;
	size_t i;

	if (chain == NULL)
		return (0);

	error = pthread_rwlock_wrlock(&chain->lock);
	if (error != 0)
		return (error);
	first_error = 0;
	for (i = 0; i < chain->image_count; i++) {
		error = scorpi_image_close_image(chain->images[i]);
		if (first_error == 0 && error != 0)
			first_error = error;
	}
	free(chain->images);
	chain->images = NULL;
	chain->image_count = 0;
	pthread_rwlock_unlock(&chain->lock);
	pthread_mutex_destroy(&chain->cache_mtx);
	pthread_rwlock_destroy(&chain->lock);
	free(chain);
	return (first_error);
}
