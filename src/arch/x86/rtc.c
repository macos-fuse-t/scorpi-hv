/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2011 NetApp, Inc.
 * All rights reserved.
 * Copyright (c) 2026 Alex Fishman <alex@fuse-t.org>
 */

#include <sys/param.h>
#include <sys/types.h>

#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include <vmmapi.h>

#include "arch/x86/inout.h"
#include "arch/x86/rtc.h"
#include "config.h"

#define	IO_RTC_ADDR	0x70
#define	IO_RTC_DATA	0x71

#define	RTC_SEC		0x00
#define	RTC_MIN		0x02
#define	RTC_HOUR	0x04
#define	RTC_WDAY	0x06
#define	RTC_MDAY	0x07
#define	RTC_MONTH	0x08
#define	RTC_YEAR	0x09
#define	RTC_STATUSA	0x0a
#define	RTC_STATUSB	0x0b
#define	RTC_INTR	0x0c
#define	RTC_STATUSD	0x0d
#define	RTC_CENTURY	0x32

#define	RTC_LMEM_LSB	0x34
#define	RTC_LMEM_MSB	0x35
#define	RTC_HMEM_LSB	0x5b
#define	RTC_HMEM_SB	0x5c
#define	RTC_HMEM_MSB	0x5d

#define	RTC_NVRAM_SIZE	128
#define	RTC_STATUSA_DV	0x20
#define	RTC_STATUSA_UIP	0x80
#define	RTC_STATUSB_24HR	0x02
#define	RTC_STATUSB_BINARY	0x04
#define	RTC_STATUSD_VRT	0x80

#define	m_64KB		(64 * 1024)
#define	m_16MB		(16 * 1024 * 1024)

static struct {
	uint8_t index;
	uint8_t nvram[RTC_NVRAM_SIZE];
	time_t base;
	struct timespec mono;
	bool inited;
} rtc;

static uint8_t
rtc_bin2bcd(int value)
{
	return (((value / 10) << 4) | (value % 10));
}

static bool
rtc_binary(void)
{
	return ((rtc.nvram[RTC_STATUSB] & RTC_STATUSB_BINARY) != 0);
}

static uint8_t
rtc_timeval(int value)
{
	return (rtc_binary() ? value : rtc_bin2bcd(value));
}

static time_t
rtc_hosttime(void)
{
	struct timespec now;

	if (!rtc.inited)
		return (time(NULL));

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (rtc.base + now.tv_sec - rtc.mono.tv_sec);
}

static void
rtc_tm(struct tm *tm)
{
	time_t now;

	now = rtc_hosttime();
	if (get_config_bool_default("rtc.use_localtime", false))
		localtime_r(&now, tm);
	else
		gmtime_r(&now, tm);
}

int
x86_rtc_settime(time_t secs)
{
	rtc.base = secs;
	clock_gettime(CLOCK_MONOTONIC, &rtc.mono);
	rtc.inited = true;
	return (0);
}

int
x86_rtc_gettime(time_t *secs)
{
	*secs = rtc_hosttime();
	return (0);
}

int
x86_rtc_write(int offset, uint8_t value)
{
	if (offset < 0 || offset >= RTC_NVRAM_SIZE)
		return (-1);

	rtc.nvram[offset] = value;
	return (0);
}

int
x86_rtc_read(int offset, uint8_t *retval)
{
	struct tm tm;

	if (offset < 0 || offset >= RTC_NVRAM_SIZE) {
		*retval = 0xff;
		return (-1);
	}

	switch (offset) {
	case RTC_SEC:
	case RTC_MIN:
	case RTC_HOUR:
	case RTC_WDAY:
	case RTC_MDAY:
	case RTC_MONTH:
	case RTC_YEAR:
	case RTC_CENTURY:
		rtc_tm(&tm);
		switch (offset) {
		case RTC_SEC:
			*retval = rtc_timeval(tm.tm_sec);
			break;
		case RTC_MIN:
			*retval = rtc_timeval(tm.tm_min);
			break;
		case RTC_HOUR:
			*retval = rtc_timeval(tm.tm_hour);
			break;
		case RTC_WDAY:
			*retval = rtc_timeval(tm.tm_wday + 1);
			break;
		case RTC_MDAY:
			*retval = rtc_timeval(tm.tm_mday);
			break;
		case RTC_MONTH:
			*retval = rtc_timeval(tm.tm_mon + 1);
			break;
		case RTC_YEAR:
			*retval = rtc_timeval(tm.tm_year % 100);
			break;
		case RTC_CENTURY:
			*retval = rtc_timeval((tm.tm_year + 1900) / 100);
			break;
		}
		return (0);
	case RTC_STATUSA:
	{
		struct timespec now;
		uint8_t uip;

		clock_gettime(CLOCK_REALTIME, &now);
		uip = now.tv_nsec >= 990000000 ? RTC_STATUSA_UIP : 0;
		*retval = (rtc.nvram[RTC_STATUSA] & ~RTC_STATUSA_UIP) | uip;
		return (0);
	}
	case RTC_INTR:
		*retval = rtc.nvram[RTC_INTR];
		rtc.nvram[RTC_INTR] = 0;
		return (0);
	default:
		*retval = rtc.nvram[offset];
		return (0);
	}
}

static int
rtc_addr_handler(struct vmctx *ctx __unused, int in, int port __unused,
    int bytes, uint32_t *eax, void *arg __unused)
{
	if (bytes != 1)
		return (-1);

	if (in)
		*eax = rtc.index;
	else
		rtc.index = *eax & 0x7f;

	return (0);
}

static int
rtc_data_handler(struct vmctx *ctx __unused, int in, int port __unused,
    int bytes, uint32_t *eax, void *arg __unused)
{
	uint8_t value;

	if (bytes != 1)
		return (-1);

	if (in) {
		if (x86_rtc_read(rtc.index, &value) != 0)
			value = 0xff;
		*eax = value;
	} else {
		(void)x86_rtc_write(rtc.index, *eax);
	}

	return (0);
}

void
x86_rtc_init(struct vmctx *ctx)
{
	size_t himem;
	size_t lomem;

	memset(&rtc, 0, sizeof(rtc));
	rtc.nvram[RTC_STATUSA] = RTC_STATUSA_DV | 0x06;
	rtc.nvram[RTC_STATUSB] = RTC_STATUSB_24HR;
	rtc.nvram[RTC_STATUSD] = RTC_STATUSD_VRT;

	if (vm_get_lowmem_size(ctx) > m_16MB)
		lomem = (vm_get_lowmem_size(ctx) - m_16MB) / m_64KB;
	else
		lomem = 0;
	assert(x86_rtc_write(RTC_LMEM_LSB, lomem) == 0);
	assert(x86_rtc_write(RTC_LMEM_MSB, lomem >> 8) == 0);

	himem = vm_get_highmem_size(ctx) / m_64KB;
	assert(x86_rtc_write(RTC_HMEM_LSB, himem) == 0);
	assert(x86_rtc_write(RTC_HMEM_SB, himem >> 8) == 0);
	assert(x86_rtc_write(RTC_HMEM_MSB, himem >> 16) == 0);

	assert(x86_rtc_settime(time(NULL)) == 0);
}

INOUT_PORT(rtc_addr, IO_RTC_ADDR, IOPORT_F_INOUT, rtc_addr_handler);
INOUT_PORT(rtc_data, IO_RTC_DATA, IOPORT_F_INOUT, rtc_data_handler);
