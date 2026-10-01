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

#ifndef NETVIRT_WG_CRYPTO_H
#define NETVIRT_WG_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The primitives WireGuard is built from.  BLAKE2s is implemented here
 * (WireGuard needs keyed hashing with variable output, which the common
 * libraries do not expose); X25519 and ChaCha20-Poly1305 are taken from
 * a user-space crypto library, resolved with dlopen() exactly like the
 * netfs backends resolve theirs, so uvroot never links against it.
 */

/* Load libcrypto.  Returns true when X25519 and ChaCha20-Poly1305 are
 * available, false otherwise (the caller then reports the bridge as
 * unavailable instead of silently sending clear text).  */
extern bool wg_crypto_init(void);
extern bool wg_crypto_ready(void);
extern const char *wg_crypto_error(void);

/* BLAKE2s, keyed or not, with an arbitrary output length (1..32).  */
extern void wg_blake2s(uint8_t *out, size_t outlen,
		       const void *in, size_t inlen,
		       const void *key, size_t keylen);

/* HMAC-BLAKE2s, 32-byte output.  */
extern void wg_hmac_blake2s(uint8_t out[32], const uint8_t *key,
			    size_t keylen, const void *in, size_t inlen);

/* WireGuard's HKDF-based KDF: derives @count (1..3) 32-byte keys.  */
extern void wg_kdf(uint8_t out[3][32], unsigned int count,
		   const uint8_t chaining_key[32],
		   const void *input, size_t inlen);

/* X25519: public key from a private key, and the shared secret.  */
extern int wg_x25519_public(uint8_t public_key[32], const uint8_t private_key[32]);
extern int wg_x25519(uint8_t shared[32], const uint8_t private_key[32],
		     const uint8_t public_key[32]);

/* ChaCha20-Poly1305 with WireGuard's 64-bit counter in a 96-bit nonce.
 * @out must have room for @plain_length + 16 bytes.  Returns the number
 * of bytes written, or -1.  Decrypt returns the plaintext length or -1
 * when the tag does not verify.  */
extern int wg_aead_encrypt(uint8_t *out, const uint8_t key[32],
			   uint64_t counter, const void *plain,
			   size_t plain_length, const void *aad,
			   size_t aad_length);
extern int wg_aead_decrypt(uint8_t *out, const uint8_t key[32],
			   uint64_t counter, const void *cipher,
			   size_t cipher_length, const void *aad,
			   size_t aad_length);

/* WireGuard's 12-byte TAI64N timestamp.  */
extern void wg_tai64n(uint8_t out[12]);

/* Generate 32 random bytes (getrandom/urandom).  */
extern int wg_random(uint8_t *out, size_t length);

#endif				/* NETVIRT_WG_CRYPTO_H */
