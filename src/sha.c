/*** sha.c -- sha calc
 *
 * Copyright (C) 2013-2017 Sebastian Freundt
 *
 * Author:  Sebastian Freundt <freundt@ga-group.nl>
 *
 * This file is part of unserlob.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the author nor the names of any contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ***/
#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <stdint.h>
#include <string.h>
#include "sha.h"
#include "nifty.h"

typedef struct {
	uint32_t v[5U];
} _sha_t;


static inline __attribute__((const, pure)) uint32_t
rotl(uint32_t x, unsigned int n)
{
	n %= 32U;
	return (x << n) | (x >> (32 - n));
}

static void
sha_chunk(_sha_t *restrict h, const uint8_t buf[static 64U])
{
	uint32_t s32[80U];
	_sha_t res = *h;
#define a	res.v[0U]
#define b	res.v[1U]
#define c	res.v[2U]
#define d	res.v[3U]
#define e	res.v[4U]

	/* extend the sixteen 32-bit words that make up BUF into 80 words */
	for (size_t i = 0U; i < 16U; i++) {
		s32[i] = 0U;
		s32[i] ^= buf[4U * i + 0U];
		s32[i] <<= 8U;
		s32[i] ^= buf[4U * i + 1U];
		s32[i] <<= 8U;
		s32[i] ^= buf[4U * i + 2U];
		s32[i] <<= 8U;
		s32[i] ^= buf[4U * i + 3U];
	}
	for (size_t i = 16U; i < 32U; i++) {
		s32[i] = s32[i - 3] ^ s32[i - 8] ^ s32[i - 14] ^ s32[i - 16];
		s32[i] = rotl(s32[i], 1U);
	}
	for (size_t i = 32U; i < 80U; i++) {
		s32[i] = s32[i - 6] ^ s32[i - 16] ^ s32[i - 28] ^ s32[i - 32];
		s32[i] = rotl(s32[i], 2U);
	}

	/* main loop */
	for (size_t i = 0U; i < 80U; i++) {
		uint32_t f, k;

		switch (i / 20U) {
		case 0U/*0U ... 19U*/:
			f = d ^ (b & (c ^ d));
			k = 0x5a827999u;
			break;
		case 1u/*20u ... 39u*/:
			f = b ^ c ^ d;
			k = 0x6ed9eba1u;
			break;
		case 2u/*40u ... 59u*/:
			f = (b & c) | (b & d) | (c & d);
			k = 0x8f1bbcdcu;
			break;
		case 3u/*60u ... 79u*/:
			f = b ^ c ^ d;
			k = 0xca62c1d6u;
			break;
		default:
			/* cpu broken! */
			break;
		}

		with (uint32_t tmp = rotl(a, 5U) + f + e + k + s32[i]) {
			e = d;
			d = c;
			c = rotl(b, 30U);
			b = a;
			a = tmp;
		}
	}

	/* add to old values */
	for (size_t i = 0U; i < 5U; i++) {
		h->v[i] += res.v[i];
	}
#undef a
#undef b
#undef c
#undef d
#undef e
	return;
}

static void
sha_fin(_sha_t *restrict h, const uint8_t buf[static 64U], size_t bsz)
{
	const size_t rsz = bsz % 64U;
	uint8_t l[64U];

	/* copy the beef, we use the fact that mmap() transparently
	 * gives us 0-bytes after the end of the map */
	memcpy(l, buf, rsz);
	memset(l + rsz, 0, sizeof(l) - rsz);
	/* append sep char 0x80 */
	l[rsz] = 0x80U;
	/* check if there's room for the filesize indicator */
	if (UNLIKELY(rsz >= 56U)) {
		/* send him off first */
		sha_chunk(h, l);
		/* wipe the buffer again */
		memset(l, 0, sizeof(l));
	}
	/* last 2 l entries are big-endian buffer size */
	with (uint64_t b64 = bsz * 8U) {
		l[64U - 8U] = (uint8_t)(b64 >> 56U);
		l[64U - 7U] = (uint8_t)(b64 >> 48U);
		l[64U - 6U] = (uint8_t)(b64 >> 40U);
		l[64U - 5U] = (uint8_t)(b64 >> 32U);
		l[64U - 4U] = (uint8_t)(b64 >> 24U);
		l[64U - 3U] = (uint8_t)(b64 >> 16U);
		l[64U - 2U] = (uint8_t)(b64 >> 8U);
		l[64U - 1U] = (uint8_t)(b64 >> 0U);
	}
	return sha_chunk(h, l);
}

int
sha(sha_t *restrict tgt, const char *buf, size_t bsz)
{
	int rc = -1;
	_sha_t h = {
		.v = {
			0x67452301U,
			0xEFCDAB89U,
			0x98BADCFEU,
			0x10325476U,
			0xC3D2E1F0U,
		}
	};

	if (UNLIKELY(bsz == 0U)) {
		sha_chunk(&h, (uint8_t[64U]){0x80U});
		goto out;
	}
	/* process 64b chunks */
	for (size_t j = 0U; j + 64U <= bsz; j += 64U) {
		sha_chunk(&h, (const uint8_t*)buf + j);
	}
	/* and the very last block */
	with (size_t rest = bsz % 64U) {
		sha_fin(&h, (const uint8_t*)buf + bsz - rest, bsz);
	}
out:
	/* success then? */
	rc = 0;
	/* convert to big endian */
	for (size_t i = 0U; i < 5U; i++) {
		(*tgt)[4U * i + 0U] = (uint8_t)(h.v[i] >> 24U);
		(*tgt)[4U * i + 1U] = (uint8_t)(h.v[i] >> 16U);
		(*tgt)[4U * i + 2U] = (uint8_t)(h.v[i] >> 8U);
		(*tgt)[4U * i + 3U] = (uint8_t)(h.v[i] >> 0U);
	}
	return rc;
}

/* sha.c ends here */
