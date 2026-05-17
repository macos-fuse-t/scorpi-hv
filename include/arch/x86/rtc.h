/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#ifndef _ARCH_X86_RTC_H_
#define	_ARCH_X86_RTC_H_

#include <stdint.h>
#include <time.h>

struct vmctx;

void	x86_rtc_init(struct vmctx *ctx);
int	x86_rtc_write(int offset, uint8_t value);
int	x86_rtc_read(int offset, uint8_t *retval);
int	x86_rtc_settime(time_t secs);
int	x86_rtc_gettime(time_t *secs);

#endif /* _ARCH_X86_RTC_H_ */
