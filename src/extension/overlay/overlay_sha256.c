/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Minimal SHA-256 implementation for the overlay database.  It is not
 * performance critical: it hashes files when a block delta is stored or
 * verified, and bkc/bme use the same code.
 */
#include <string.h>

#include "extension/overlay/overlay_sha256.h"

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t ror(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

static void transform(OverlaySha256 *ctx, const unsigned char block[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    unsigned i;

    for (i = 0; i < 16; i++) {
	w[i] = ((uint32_t) block[i * 4] << 24)
	    | ((uint32_t) block[i * 4 + 1] << 16)
	    | ((uint32_t) block[i * 4 + 2] << 8)
	    | (uint32_t) block[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
	uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
	uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);

	w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; i++) {
	uint32_t s1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
	uint32_t ch = (e & f) ^ (~e & g);
	uint32_t t1 = h + s1 + ch + K[i] + w[i];
	uint32_t s0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
	uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
	uint32_t t2 = s0 + maj;

	h = g;
	g = f;
	f = e;
	e = d + t1;
	d = c;
	c = b;
	b = a;
	a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void overlay_sha256_init(OverlaySha256 *ctx)
{
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->length = 0;
    ctx->used = 0;
}

void overlay_sha256_update(OverlaySha256 *ctx, const void *data, size_t size)
{
    const unsigned char *p = data;
    size_t left = size;

    ctx->length += size;
    if (ctx->used != 0) {
	size_t room = 64 - ctx->used;
	size_t take = left < room ? left : room;

	memcpy(ctx->buffer + ctx->used, p, take);
	ctx->used += take;
	p += take;
	left -= take;
	if (ctx->used == 64) {
	    transform(ctx, ctx->buffer);
	    ctx->used = 0;
	}
    }
    while (left >= 64) {
	transform(ctx, p);
	p += 64;
	left -= 64;
    }
    if (left != 0) {
	memcpy(ctx->buffer, p, left);
	ctx->used = left;
    }
}

void overlay_sha256_final(OverlaySha256 *ctx, unsigned char digest[32])
{
    uint64_t bits = ctx->length * 8;
    unsigned char pad[72];
    size_t padlen;
    unsigned i;

    pad[0] = 0x80;
    memset(pad + 1, 0, sizeof(pad) - 1);
    padlen = (ctx->used < 56) ? (56 - ctx->used) : (120 - ctx->used);
    overlay_sha256_update(ctx, pad, padlen);

    for (i = 0; i < 8; i++)
	pad[i] = (unsigned char) (bits >> (56 - i * 8));
    overlay_sha256_update(ctx, pad, 8);

    for (i = 0; i < 8; i++) {
	digest[i * 4] = (unsigned char) (ctx->state[i] >> 24);
	digest[i * 4 + 1] = (unsigned char) (ctx->state[i] >> 16);
	digest[i * 4 + 2] = (unsigned char) (ctx->state[i] >> 8);
	digest[i * 4 + 3] = (unsigned char) ctx->state[i];
    }
}

void overlay_sha256_hex(const unsigned char digest[32], char hex[65])
{
    static const char digits[] = "0123456789abcdef";
    unsigned i;

    for (i = 0; i < 32; i++) {
	hex[i * 2] = digits[digest[i] >> 4];
	hex[i * 2 + 1] = digits[digest[i] & 0xf];
    }
    hex[64] = '\0';
}
