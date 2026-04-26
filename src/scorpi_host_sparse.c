/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/errno.h>
#include <sys/types.h>

#include <stdint.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <fcntl.h>
#endif

#if defined(__linux__)
#include <fcntl.h>
#include <linux/falloc.h>
#include <sys/syscall.h>
#endif

#include "scorpi_host_sparse.h"

int
scorpi_host_punch_hole(int fd, uint64_t offset, uint64_t length)
{
	if (fd < 0)
		return (EINVAL);
	if (length == 0)
		return (0);
	if (offset > INT64_MAX || length > INT64_MAX)
		return (EOVERFLOW);

#if defined(__APPLE__) && defined(F_PUNCHHOLE)
	fpunchhole_t punch;

	punch = (fpunchhole_t){
		.fp_offset = (off_t)offset,
		.fp_length = (off_t)length,
	};
	if (fcntl(fd, F_PUNCHHOLE, &punch) == 0)
		return (0);
	return (errno);
#elif defined(__linux__) && defined(SYS_fallocate)
	if (syscall(SYS_fallocate, fd,
	    FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
	    (off_t)offset, (off_t)length) == 0)
		return (0);
	return (errno);
#else
	return (EOPNOTSUPP);
#endif
}
