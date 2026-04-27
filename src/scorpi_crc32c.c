/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "scorpi_crc32c.h"

#include <string.h>

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <nmmintrin.h>
#endif

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
static uint32_t
scorpi_crc32c_arm64(const void *buf, size_t len)
{
	const uint8_t *p;
	uint64_t word64;
	uint32_t word32;
	uint16_t word16;
	uint32_t crc;

	p = buf;
	crc = 0xffffffffU;
	while (len >= sizeof(word64)) {
		memcpy(&word64, p, sizeof(word64));
		crc = __crc32cd(crc, word64);
		p += sizeof(word64);
		len -= sizeof(word64);
	}
	if (len >= sizeof(word32)) {
		memcpy(&word32, p, sizeof(word32));
		crc = __crc32cw(crc, word32);
		p += sizeof(word32);
		len -= sizeof(word32);
	}
	if (len >= sizeof(word16)) {
		memcpy(&word16, p, sizeof(word16));
		crc = __crc32ch(crc, word16);
		p += sizeof(word16);
		len -= sizeof(word16);
	}
	if (len != 0)
		crc = __crc32cb(crc, *p);
	return (~crc);
}
#else
static uint32_t
scorpi_crc32c_soft(const void *buf, size_t len)
{
	const uint8_t *p;
	uint32_t crc;
	size_t i;
	int bit;

	p = buf;
	crc = 0xffffffffU;
	for (i = 0; i < len; i++) {
		crc ^= p[i];
		for (bit = 0; bit < 8; bit++) {
			if ((crc & 1) != 0)
				crc = (crc >> 1) ^ 0x82f63b78U;
			else
				crc >>= 1;
		}
	}
	return (~crc);
}
#endif

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("sse4.2")))
static uint32_t
scorpi_crc32c_sse42(const void *buf, size_t len)
{
	const uint8_t *p;
#if defined(__x86_64__)
	uint64_t word64;
#endif
	uint32_t word32;
	uint32_t crc;

	p = buf;
	crc = 0xffffffffU;
#if defined(__x86_64__)
	while (len >= sizeof(word64)) {
		memcpy(&word64, p, sizeof(word64));
		crc = (uint32_t)_mm_crc32_u64(crc, word64);
		p += sizeof(word64);
		len -= sizeof(word64);
	}
#endif
	while (len >= sizeof(word32)) {
		memcpy(&word32, p, sizeof(word32));
		crc = _mm_crc32_u32(crc, word32);
		p += sizeof(word32);
		len -= sizeof(word32);
	}
	while (len != 0) {
		crc = _mm_crc32_u8(crc, *p);
		p++;
		len--;
	}
	return (~crc);
}
#endif

uint32_t
scorpi_crc32c(const void *buf, size_t len)
{
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
	return (scorpi_crc32c_arm64(buf, len));
#elif defined(__x86_64__) || defined(__i386__)
	if (__builtin_cpu_supports("sse4.2"))
		return (scorpi_crc32c_sse42(buf, len));
	return (scorpi_crc32c_soft(buf, len));
#else
	return (scorpi_crc32c_soft(buf, len));
#endif
}
