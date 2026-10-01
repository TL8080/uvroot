/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of uvroot.
 *
 * Copyright (C) 2026 uvroot Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/random.h>
#include <sys/time.h>
#include <sys/types.h>

#include "wg_crypto.h"

/* ------------------------------------------------------------------ */
/* BLAKE2s (RFC 7693)                                                  */
/* ------------------------------------------------------------------ */

#define BLAKE2S_BLOCK 64

typedef struct {
    uint32_t h[8];
    uint32_t t[2];
    uint32_t f[2];
    uint8_t buffer[BLAKE2S_BLOCK];
    size_t buffer_length;
    size_t output_length;
} Blake2s;

static const uint32_t blake2s_iv[8] = {
    0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
    0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19,
};

static const uint8_t blake2s_sigma[10][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
    { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
    { 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
    { 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
    { 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
    { 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
    { 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
    { 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
    { 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
    { 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
};

static inline uint32_t load32(const uint8_t *p)
{
    return (uint32_t) p[0]
	| ((uint32_t) p[1] << 8)
	| ((uint32_t) p[2] << 16)
	| ((uint32_t) p[3] << 24);
}

static inline void store32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16);
    p[3] = (uint8_t) (v >> 24);
}

static inline uint32_t rotate_right(uint32_t v, int n)
{
    return (v >> n) | (v << (32 - n));
}

#define BLAKE2S_G(a, b, c, d, x, y)			\
    do {						\
	v[a] = v[a] + v[b] + (x);			\
	v[d] = rotate_right(v[d] ^ v[a], 16);		\
	v[c] = v[c] + v[d];				\
	v[b] = rotate_right(v[b] ^ v[c], 12);		\
	v[a] = v[a] + v[b] + (y);			\
	v[d] = rotate_right(v[d] ^ v[a], 8);		\
	v[c] = v[c] + v[d];				\
	v[b] = rotate_right(v[b] ^ v[c], 7);		\
    } while (0)

static void blake2s_compress(Blake2s *state, const uint8_t block[BLAKE2S_BLOCK])
{
    uint32_t m[16];
    uint32_t v[16];
    int i;

    for (i = 0; i < 16; i++)
	m[i] = load32(block + 4 * i);
    for (i = 0; i < 8; i++)
	v[i] = state->h[i];
    for (i = 0; i < 8; i++)
	v[i + 8] = blake2s_iv[i];

    v[12] ^= state->t[0];
    v[13] ^= state->t[1];
    v[14] ^= state->f[0];
    v[15] ^= state->f[1];

    for (i = 0; i < 10; i++) {
	const uint8_t *s = blake2s_sigma[i];

	BLAKE2S_G(0, 4, 8, 12, m[s[0]], m[s[1]]);
	BLAKE2S_G(1, 5, 9, 13, m[s[2]], m[s[3]]);
	BLAKE2S_G(2, 6, 10, 14, m[s[4]], m[s[5]]);
	BLAKE2S_G(3, 7, 11, 15, m[s[6]], m[s[7]]);
	BLAKE2S_G(0, 5, 10, 15, m[s[8]], m[s[9]]);
	BLAKE2S_G(1, 6, 11, 12, m[s[10]], m[s[11]]);
	BLAKE2S_G(2, 7, 8, 13, m[s[12]], m[s[13]]);
	BLAKE2S_G(3, 4, 9, 14, m[s[14]], m[s[15]]);
    }

    for (i = 0; i < 8; i++)
	state->h[i] ^= v[i] ^ v[i + 8];
}

static void blake2s_increment(Blake2s *state, uint32_t amount)
{
    state->t[0] += amount;
    if (state->t[0] < amount)
	state->t[1]++;
}

static void blake2s_init(Blake2s *state, size_t output_length,
			 const void *key, size_t key_length)
{
    memset(state, 0, sizeof(*state));
    state->output_length = output_length;
    /* digest_length | key_length << 8 | fanout(1) << 16 | depth(2) << 24 */
    state->h[0] = blake2s_iv[0]
	^ (uint32_t) (0x01010000u
		      ^ ((uint32_t) key_length << 8)
		      ^ (uint32_t) output_length);
    memcpy(&state->h[1], &blake2s_iv[1], 7 * sizeof(uint32_t));

    if (key != NULL && key_length > 0) {
	uint8_t block[BLAKE2S_BLOCK];

	memset(block, 0, sizeof(block));
	memcpy(block, key, key_length);
	/* The padded key is the first block; keeping it buffered (and
	 * not compressed yet) is what makes it the last block too when
	 * the message is empty, which is the BLAKE2 keyed mode.  */
	memcpy(state->buffer, block, BLAKE2S_BLOCK);
	state->buffer_length = BLAKE2S_BLOCK;
    }
}

static void blake2s_update(Blake2s *state, const void *input, size_t length)
{
    const uint8_t *in = input;

    if (length == 0)
	return;

    {
	size_t left = state->buffer_length;
	size_t fill = BLAKE2S_BLOCK - left;

	if (length > fill) {
	    state->buffer_length = 0;
	    memcpy(state->buffer + left, in, fill);
	    blake2s_increment(state, BLAKE2S_BLOCK);
	    blake2s_compress(state, state->buffer);
	    in += fill;
	    length -= fill;
	    while (length > BLAKE2S_BLOCK) {
		blake2s_increment(state, BLAKE2S_BLOCK);
		blake2s_compress(state, in);
		in += BLAKE2S_BLOCK;
		length -= BLAKE2S_BLOCK;
	    }
	}
	memcpy(state->buffer + state->buffer_length, in, length);
	state->buffer_length += length;
    }
}

static void blake2s_final(Blake2s *state, uint8_t *output)
{
    uint8_t block[BLAKE2S_BLOCK];

    blake2s_increment(state, (uint32_t) state->buffer_length);
    state->f[0] = 0xFFFFFFFFu;
    memset(block, 0, sizeof(block));
    memcpy(block, state->buffer, state->buffer_length);
    blake2s_compress(state, block);

    {
	size_t i;
	for (i = 0; i < state->output_length; i++)
	    output[i] = ((uint8_t *) state->h)[i];
    }
}

void wg_blake2s(uint8_t *out, size_t outlen, const void *in, size_t inlen,
		const void *key, size_t keylen)
{
    Blake2s state;

    if (outlen == 0 || outlen > 32)
	return;
    blake2s_init(&state, outlen, key, keylen);
    blake2s_update(&state, in, inlen);
    blake2s_final(&state, out);
}

/* ------------------------------------------------------------------ */
/* HMAC-BLAKE2s and the WireGuard KDF                                  */
/* ------------------------------------------------------------------ */

void wg_hmac_blake2s(uint8_t out[32], const uint8_t *key, size_t keylen,
		     const void *in, size_t inlen)
{
    uint8_t normalized[BLAKE2S_BLOCK];
    uint8_t ipad[BLAKE2S_BLOCK];
    uint8_t opad[BLAKE2S_BLOCK];
    uint8_t inner[32];
    Blake2s state;
    size_t i;

    memset(normalized, 0, sizeof(normalized));
    if (keylen > BLAKE2S_BLOCK) {
	uint8_t hashed[32];

	wg_blake2s(hashed, 32, key, keylen, NULL, 0);
	memcpy(normalized, hashed, 32);
    } else if (keylen > 0)
	memcpy(normalized, key, keylen);

    for (i = 0; i < BLAKE2S_BLOCK; i++) {
	ipad[i] = normalized[i] ^ 0x36;
	opad[i] = normalized[i] ^ 0x5c;
    }

    blake2s_init(&state, 32, NULL, 0);
    blake2s_update(&state, ipad, sizeof(ipad));
    blake2s_update(&state, in, inlen);
    blake2s_final(&state, inner);

    blake2s_init(&state, 32, NULL, 0);
    blake2s_update(&state, opad, sizeof(opad));
    blake2s_update(&state, inner, sizeof(inner));
    blake2s_final(&state, out);
}

void wg_kdf(uint8_t out[3][32], unsigned int count,
	    const uint8_t chaining_key[32], const void *input, size_t inlen)
{
    uint8_t t0[32];
    uint8_t buffer[32 + 1];
    unsigned int i;

    wg_hmac_blake2s(t0, chaining_key, 32, input, inlen);

    for (i = 1; i <= count && i <= 3; i++) {
	if (i == 1) {
	    buffer[0] = 1;
	    wg_hmac_blake2s(out[0], t0, 32, buffer, 1);
	} else {
	    memcpy(buffer, out[i - 2], 32);
	    buffer[32] = (uint8_t) i;
	    wg_hmac_blake2s(out[i - 1], t0, 32, buffer, 33);
	}
    }
}

/* ------------------------------------------------------------------ */
/* TAI64N and randomness                                               */
/* ------------------------------------------------------------------ */

void wg_tai64n(uint8_t out[12])
{
    struct timeval now;
    uint64_t seconds;
    uint32_t nanoseconds;
    int i;

    gettimeofday(&now, NULL);
    seconds = (uint64_t) now.tv_sec + 0x4000000000000000ULL;
    nanoseconds = (uint32_t) (now.tv_usec * 1000);

    for (i = 0; i < 8; i++)
	out[i] = (uint8_t) (seconds >> (56 - 8 * i));
    for (i = 0; i < 4; i++)
	out[8 + i] = (uint8_t) (nanoseconds >> (24 - 8 * i));
}

int wg_random(uint8_t *out, size_t length)
{
    size_t done = 0;

#ifdef SYS_getrandom
    while (done < length) {
	ssize_t got = getrandom(out + done, length - done, 0);

	if (got < 0) {
	    if (errno == EINTR)
		continue;
	    break;
	}
	done += (size_t) got;
    }
    if (done == length)
	return 0;
#endif

    {
	int fd = open("/dev/urandom", O_RDONLY);

	if (fd < 0)
	    return -1;
	while (done < length) {
	    ssize_t got = read(fd, out + done, length - done);

	    if (got < 0) {
		if (errno == EINTR)
		    continue;
		close(fd);
		return -1;
	    }
	    if (got == 0)
		break;
	    done += (size_t) got;
	}
	close(fd);
    }
    return (done == length) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* X25519 and ChaCha20-Poly1305, from a user-space library             */
/* ------------------------------------------------------------------ */

static bool crypto_ready = false;
static const char *crypto_error = "libcrypto not loaded";

#ifdef HAVE_LIBCRYPTO

#include <dlfcn.h>
#include <openssl/evp.h>

#define CRYPTO_SYMBOLS(X)						\
    X(EVP_PKEY_new_raw_private_key)					\
    X(EVP_PKEY_new_raw_public_key)					\
    X(EVP_PKEY_get_raw_public_key)					\
    X(EVP_PKEY_free)							\
    X(EVP_PKEY_CTX_new)							\
    X(EVP_PKEY_CTX_free)						\
    X(EVP_PKEY_derive_init)						\
    X(EVP_PKEY_derive_set_peer)						\
    X(EVP_PKEY_derive)							\
    X(EVP_CIPHER_CTX_new)						\
    X(EVP_CIPHER_CTX_free)						\
    X(EVP_EncryptInit_ex)						\
    X(EVP_EncryptUpdate)						\
    X(EVP_EncryptFinal_ex)						\
    X(EVP_DecryptInit_ex)						\
    X(EVP_DecryptUpdate)						\
    X(EVP_DecryptFinal_ex)						\
    X(EVP_CIPHER_CTX_ctrl)						\
    X(EVP_chacha20_poly1305)

#define DECLARE(name) static __typeof__(&name) p_##name;
CRYPTO_SYMBOLS(DECLARE)
#undef DECLARE

static void *crypto_handle = NULL;

static void *try_open(const char *name)
{
    void *handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);

    if (handle == NULL) {
	const char *prefix = getenv("PREFIX");

	if (prefix != NULL) {
	    char path[4096];

	    snprintf(path, sizeof(path), "%s/lib/%s", prefix, name);
	    handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	}
    }
    return handle;
}

bool wg_crypto_init(void)
{
    static const char *names[] = {
	"libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so", NULL
    };
    int i;

    if (crypto_ready)
	return true;
    if (crypto_handle != NULL)
	return false;

    for (i = 0; names[i] != NULL && crypto_handle == NULL; i++)
	crypto_handle = try_open(names[i]);

    if (crypto_handle == NULL) {
	crypto_error = "libcrypto.so not found (WireGuard needs it)";
	return false;
    }

#define LOAD(name)							\
    do {								\
	*(void **) (&p_##name) = dlsym(crypto_handle, #name);		\
	if (p_##name == NULL) {						\
	    crypto_error = "libcrypto is missing " #name;		\
	    dlclose(crypto_handle);					\
	    crypto_handle = NULL;					\
	    return false;						\
	}								\
    } while (0);
    CRYPTO_SYMBOLS(LOAD)
#undef LOAD

    crypto_ready = true;
    crypto_error = NULL;
    return true;
}

bool wg_crypto_ready(void)
{
    return crypto_ready;
}

const char *wg_crypto_error(void)
{
    return crypto_error;
}

int wg_x25519_public(uint8_t public_key[32], const uint8_t private_key[32])
{
    EVP_PKEY *key;
    size_t length = 32;
    int status;

    if (!crypto_ready)
	return -1;
    key = p_EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL,
					 private_key, 32);
    if (key == NULL)
	return -1;
    status = p_EVP_PKEY_get_raw_public_key(key, public_key, &length);
    p_EVP_PKEY_free(key);
    return (status == 1 && length == 32) ? 0 : -1;
}

int wg_x25519(uint8_t shared[32], const uint8_t private_key[32],
	      const uint8_t public_key[32])
{
    EVP_PKEY *own;
    EVP_PKEY *peer;
    EVP_PKEY_CTX *context;
    size_t length = 32;
    int status = -1;

    if (!crypto_ready)
	return -1;
    own = p_EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL,
					 private_key, 32);
    peer = p_EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL,
					 public_key, 32);
    if (own == NULL || peer == NULL)
	goto out;
    context = p_EVP_PKEY_CTX_new(own, NULL);
    if (context == NULL)
	goto out;
    if (p_EVP_PKEY_derive_init(context) == 1
	&& p_EVP_PKEY_derive_set_peer(context, peer) == 1
	&& p_EVP_PKEY_derive(context, shared, &length) == 1
	&& length == 32)
	status = 0;
    p_EVP_PKEY_CTX_free(context);
  out:
    if (own != NULL)
	p_EVP_PKEY_free(own);
    if (peer != NULL)
	p_EVP_PKEY_free(peer);
    return status;
}

static void aead_nonce(uint8_t nonce[12], uint64_t counter)
{
    int i;

    /* WireGuard puts the 64-bit counter in little-endian order after
     * four leading zero bytes (RFC 8439 nonce).  */
    memset(nonce, 0, 4);
    for (i = 0; i < 8; i++)
	nonce[4 + i] = (uint8_t) (counter >> (8 * i));
}

int wg_aead_encrypt(uint8_t *out, const uint8_t key[32], uint64_t counter,
		    const void *plain, size_t plain_length, const void *aad,
		    size_t aad_length)
{
    EVP_CIPHER_CTX *context;
    uint8_t nonce[12];
    int length = 0;
    int total = 0;

    if (!crypto_ready)
	return -1;
    context = p_EVP_CIPHER_CTX_new();
    if (context == NULL)
	return -1;

    aead_nonce(nonce, counter);
    if (p_EVP_EncryptInit_ex(context, p_EVP_chacha20_poly1305(), NULL, NULL,
			     NULL) != 1
	|| p_EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_AEAD_SET_IVLEN, 12,
				 NULL) != 1
	|| p_EVP_EncryptInit_ex(context, NULL, NULL, key, nonce) != 1)
	goto error;
    if (aad_length > 0
	&& p_EVP_EncryptUpdate(context, NULL, &length, aad,
			       (int) aad_length) != 1)
	goto error;
    /* The AAD call reports the AAD length in @length; it is not part of
     * the ciphertext, so it must not be counted.  */
    length = 0;
    if (plain_length > 0
	&& p_EVP_EncryptUpdate(context, out, &length, plain,
			       (int) plain_length) != 1)
	goto error;
    total = length;
    length = 0;
    if (p_EVP_EncryptFinal_ex(context, out + total, &length) != 1)
	goto error;
    total += length;
    if (p_EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_AEAD_GET_TAG, 16,
			      out + total) != 1)
	goto error;
    p_EVP_CIPHER_CTX_free(context);
    return total + 16;

  error:
    p_EVP_CIPHER_CTX_free(context);
    return -1;
}

int wg_aead_decrypt(uint8_t *out, const uint8_t key[32], uint64_t counter,
		    const void *cipher, size_t cipher_length, const void *aad,
		    size_t aad_length)
{
    EVP_CIPHER_CTX *context;
    uint8_t nonce[12];
    uint8_t tag[16];
    int length = 0;
    int total = 0;
    size_t data_length;

    if (!crypto_ready || cipher_length < 16)
	return -1;
    data_length = cipher_length - 16;
    memcpy(tag, (const uint8_t *) cipher + data_length, 16);

    context = p_EVP_CIPHER_CTX_new();
    if (context == NULL)
	return -1;

    aead_nonce(nonce, counter);
    if (p_EVP_DecryptInit_ex(context, p_EVP_chacha20_poly1305(), NULL, NULL,
			     NULL) != 1
	|| p_EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_AEAD_SET_IVLEN, 12,
				 NULL) != 1
	|| p_EVP_DecryptInit_ex(context, NULL, NULL, key, nonce) != 1)
	goto error;
    if (aad_length > 0
	&& p_EVP_DecryptUpdate(context, NULL, &length, aad,
			       (int) aad_length) != 1)
	goto error;
    length = 0;
    if (data_length > 0
	&& p_EVP_DecryptUpdate(context, out, &length, cipher,
			       (int) data_length) != 1)
	goto error;
    total = length;
    if (p_EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_AEAD_SET_TAG, 16, tag) != 1)
	goto error;
    length = 0;
    if (p_EVP_DecryptFinal_ex(context, out + total, &length) != 1)
	goto error;
    total += length;
    p_EVP_CIPHER_CTX_free(context);
    return total;

  error:
    p_EVP_CIPHER_CTX_free(context);
    return -1;
}

#else				/* !HAVE_LIBCRYPTO */

bool wg_crypto_init(void)
{
    crypto_ready = false;
    crypto_error = "uvroot was built without the OpenSSL headers";
    return false;
}

bool wg_crypto_ready(void)
{
    return crypto_ready;
}

const char *wg_crypto_error(void)
{
    return crypto_error;
}

int wg_x25519_public(uint8_t public_key[32], const uint8_t private_key[32])
{
    (void) public_key;
    (void) private_key;
    return -1;
}

int wg_x25519(uint8_t shared[32], const uint8_t private_key[32],
	      const uint8_t public_key[32])
{
    (void) shared;
    (void) private_key;
    (void) public_key;
    return -1;
}

int wg_aead_encrypt(uint8_t *out, const uint8_t key[32], uint64_t counter,
		    const void *plain, size_t plain_length, const void *aad,
		    size_t aad_length)
{
    (void) out; (void) key; (void) counter; (void) plain;
    (void) plain_length; (void) aad; (void) aad_length;
    return -1;
}

int wg_aead_decrypt(uint8_t *out, const uint8_t key[32], uint64_t counter,
		    const void *cipher, size_t cipher_length, const void *aad,
		    size_t aad_length)
{
    (void) out; (void) key; (void) counter; (void) cipher;
    (void) cipher_length; (void) aad; (void) aad_length;
    return -1;
}

#endif				/* HAVE_LIBCRYPTO */
