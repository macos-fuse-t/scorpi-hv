/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#ifndef _SCORPI_HOST_SPARSE_H_
#define	_SCORPI_HOST_SPARSE_H_

#include <stdint.h>

int	scorpi_host_punch_hole(int fd, uint64_t offset, uint64_t length);

#endif /* _SCORPI_HOST_SPARSE_H_ */
