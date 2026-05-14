/*-
 * SPDX-License-Identifier: Beerware
 *
 * Compatibility subset of FreeBSD sys/sys/disk.h for userland builds.
 */

#pragma once

#include <stdint.h>
#include <sys/ioccom.h>
#include <sys/param.h>
#include <sys/types.h>

#ifndef DISK_IDENT_SIZE
#define	DISK_IDENT_SIZE	256
#endif

#ifndef DIOCGSECTORSIZE
#define	DIOCGSECTORSIZE		_IOR('d', 128, u_int)
#endif
#ifndef DIOCGMEDIASIZE
#define	DIOCGMEDIASIZE		_IOR('d', 129, off_t)
#endif
#ifndef DIOCGDELETE
#define	DIOCGDELETE		_IOW('d', 136, off_t[2])
#endif
#ifndef DIOCGIDENT
#define	DIOCGIDENT		_IOR('d', 137, char[DISK_IDENT_SIZE])
#endif
#ifndef DIOCGPROVIDERNAME
#define	DIOCGPROVIDERNAME	_IOR('d', 138, char[MAXPATHLEN])
#endif
#ifndef DIOCGSTRIPESIZE
#define	DIOCGSTRIPESIZE		_IOR('d', 139, off_t)
#endif
#ifndef DIOCGSTRIPEOFFSET
#define	DIOCGSTRIPEOFFSET	_IOR('d', 140, off_t)
#endif

struct diocgattr_arg {
	char name[64];
	int len;
	union {
		char str[DISK_IDENT_SIZE];
		off_t off;
		int i;
		uint16_t u16;
	} value;
};

#ifndef DIOCGATTR
#define	DIOCGATTR		_IOWR('d', 142, struct diocgattr_arg)
#endif
