/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2016 Jakub Klama <jceel@FreeBSD.org>.
 * Copyright (c) 2018 Alexander Motin <mav@FreeBSD.org>
 * Copyright (c) 2025 Alex Fishman <alex@fuse-t.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/uio.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "iov.h"

void
seek_iov(const struct iovec *iov1, int niov1, struct iovec *iov2, int *niov2,
    size_t seek)
{
	size_t remainder = 0;
	size_t left = seek;
	int i, j;

	for (i = 0; i < niov1; i++) {
		size_t toseek = MIN(left, iov1[i].iov_len);
		left -= toseek;

		if (toseek == iov1[i].iov_len)
			continue;

		if (left == 0) {
			remainder = toseek;
			break;
		}
	}

	for (j = i; j < niov1; j++) {
		iov2[j - i].iov_base = (char *)iov1[j].iov_base + remainder;
		iov2[j - i].iov_len = iov1[j].iov_len - remainder;
		remainder = 0;
	}

	*niov2 = j - i;
}

size_t
count_iov(const struct iovec *iov, int niov)
{
	size_t total = 0;
	int i;

	for (i = 0; i < niov; i++)
		total += iov[i].iov_len;

	return (total);
}

void
truncate_iov(struct iovec *iov, int *niov, size_t length)
{
	size_t done = 0;
	int i;

	for (i = 0; i < *niov; i++) {
		size_t toseek = MIN(length - done, iov[i].iov_len);
		done += toseek;

		if (toseek <= iov[i].iov_len) {
			iov[i].iov_len = toseek;
			*niov = i + 1;
			return;
		}
	}
}

ssize_t
iov_to_buf(const struct iovec *iov, int niov, void **buf)
{
	size_t ptr, total;
	int i;

	total = count_iov(iov, niov);
	*buf = realloc(*buf, total);
	if (*buf == NULL)
		return (-1);

	for (i = 0, ptr = 0; i < niov; i++) {
		memcpy((uint8_t *)*buf + ptr, iov[i].iov_base, iov[i].iov_len);
		ptr += iov[i].iov_len;
	}

	return (total);
}

ssize_t
buf_to_iov(const void *buf, size_t buflen, const struct iovec *iov, int niov,
    size_t seek)
{
	struct iovec *diov;
	size_t off = 0, len;
	int i;

	if (seek > 0) {
		int ndiov;

		diov = malloc(sizeof(struct iovec) * niov);
		seek_iov(iov, niov, diov, &ndiov, seek);
		iov = diov;
		niov = ndiov;
	}

	for (i = 0; i < niov && off < buflen; i++) {
		len = MIN(iov[i].iov_len, buflen - off);
		memcpy(iov[i].iov_base, (const uint8_t *)buf + off, len);
		off += len;
	}

	if (seek > 0)
		free(diov);

	return ((ssize_t)off);
}

struct iovec *
iov_trim(struct iovec *iov, int *iovcnt, size_t hlen)
{
	size_t remaining = hlen;
	int count = *iovcnt;
	int i = 0;

	while (i < count && remaining > 0) {
		if (iov[i].iov_len <= remaining) {
			remaining -= iov[i].iov_len;
			i++;
		} else {
			iov[i].iov_base = (char *)iov[i].iov_base + remaining;
			iov[i].iov_len -= remaining;
			remaining = 0;
		}
	}

	*iovcnt -= i;

	return (i == count) ? NULL : &iov[i];
}

size_t
iov_copy(void *buf, size_t len, const struct iovec *iov, int iovcnt,
    size_t offset)
{
	size_t copied = 0;
	size_t remaining = offset;
	char *dst = (char *)buf;

	int i = 0;
	while (i < iovcnt && remaining >= iov[i].iov_len) {
		remaining -= iov[i].iov_len;
		i++;
	}

	if (i == iovcnt) {
		return 0;
	}

	while (i < iovcnt && copied < len) {
		size_t to_copy = (iov[i].iov_len - remaining < (len - copied)) ?
		    (iov[i].iov_len - remaining) :
		    (len - copied);

		memcpy(dst + copied, (char *)iov[i].iov_base + remaining,
		    to_copy);
		copied += to_copy;
		remaining = 0;
		i++;
	}

	return copied;
}

size_t
make_iov(const struct iovec *iov1, int niov1, struct iovec *iov2, int *niov2,
    size_t seek, size_t len)
{
	size_t remaining_seek = seek;
	size_t remaining_len = len;
	size_t total_copied = 0;
	int j = 0;

	// Find the starting point in iov1 based on seek
	int i = 0;
	while (i < niov1 && remaining_seek >= iov1[i].iov_len) {
		remaining_seek -= iov1[i].iov_len;
		i++;
	}

	// If seek exceeds total data length, return 0
	if (i == niov1) {
		*niov2 = 0;
		return 0;
	}

	// Copy relevant portions into iov2
	while (i < niov1 && remaining_len > 0) {
		size_t to_copy = (iov1[i].iov_len - remaining_seek <
				     remaining_len) ?
		    (iov1[i].iov_len - remaining_seek) :
		    remaining_len;

		iov2[j].iov_base = (char *)iov1[i].iov_base + remaining_seek;
		iov2[j].iov_len = to_copy;

		total_copied += to_copy;
		remaining_len -= to_copy;
		remaining_seek = 0;

		j++;
		i++;
	}

	*niov2 = j;
	return total_copied;
}
