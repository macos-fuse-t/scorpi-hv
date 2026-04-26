/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#ifndef _SCORPI_IMAGE_QCOW2_H_
#define _SCORPI_IMAGE_QCOW2_H_

#include "scorpi_image.h"

const struct scorpi_image_ops *scorpi_image_qcow2_backend(void);

#endif /* _SCORPI_IMAGE_QCOW2_H_ */
