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

/*
 * The WireGuard handshake: Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s, as
 * described by the WireGuard whitepaper.  Only the message layout and
 * the key schedule live here; the packets themselves are driven by
 * wg_engine.c.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "wg_crypto.h"
#include "wg_noise.h"

static const char wg_construction[] =
    "Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s";
static const char wg_identifier[] =
    "WireGuard v1 zx2c4 Jason@zx2c4.com";
static const char wg_label_mac1[] = "mac1----";

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t) value;
    p[1] = (uint8_t) (value >> 8);
    p[2] = (uint8_t) (value >> 16);
    p[3] = (uint8_t) (value >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t) p[0]
	| ((uint32_t) p[1] << 8)
	| ((uint32_t) p[2] << 16)
	| ((uint32_t) p[3] << 24);
}

static void hash_concat(uint8_t out[32], const uint8_t *first,
			size_t first_length, const uint8_t *second,
			size_t second_length)
{
    uint8_t buffer[256];

    memcpy(buffer, first, first_length);
    memcpy(buffer + first_length, second, second_length);
    wg_blake2s(out, 32, buffer, first_length + second_length, NULL, 0);
}

static bool is_zero(const uint8_t *data, size_t length)
{
    size_t i;

    for (i = 0; i < length; i++) {
	if (data[i] != 0)
	    return false;
    }
    return true;
}

void wg_noise_reset(WgNoise *state, const uint8_t peer_static[32])
{
    uint8_t buffer[32 + sizeof(wg_identifier)];

    /* Only reset the fields this function owns: the role flags, the
     * indexes and the replay window belong to the caller.  */
    memset(state->chaining_key, 0, sizeof(state->chaining_key));
    memset(state->hash, 0, sizeof(state->hash));
    memset(state->ephemeral_private, 0, sizeof(state->ephemeral_private));
    memset(state->ephemeral_public, 0, sizeof(state->ephemeral_public));

    wg_blake2s(state->chaining_key, 32, wg_construction,
	       strlen(wg_construction), NULL, 0);

    memcpy(buffer, state->chaining_key, 32);
    memcpy(buffer + 32, wg_identifier, strlen(wg_identifier));
    wg_blake2s(state->hash, 32, buffer, 32 + strlen(wg_identifier), NULL, 0);

    hash_concat(state->hash, state->hash, 32, peer_static, 32);
}

void wg_noise_mac1(uint8_t mac1[16], const uint8_t peer_static[32],
		   const uint8_t *message, size_t length)
{
    uint8_t key_input[8 + 32];
    uint8_t key[32];

    memcpy(key_input, wg_label_mac1, 8);
    memcpy(key_input + 8, peer_static, 32);
    wg_blake2s(key, 32, key_input, sizeof(key_input), NULL, 0);
    wg_blake2s(mac1, 16, message, length, key, 32);
}

bool wg_noise_timestamp_ok(WgNoise *state, const uint8_t timestamp[12])
{
    if (!is_zero(state->last_timestamp, 12)
	&& memcmp(timestamp, state->last_timestamp, 12) <= 0)
	return false;
    memcpy(state->last_timestamp, timestamp, 12);
    return true;
}

int wg_noise_create_initiation(WgNoise *state, uint32_t local_index,
			       const uint8_t local_static_private[32],
			       const uint8_t remote_static_public[32],
			       uint8_t message[WG_MSG_INITIATION_SIZE])
{
    uint8_t out[3][32];
    uint8_t dh[32];
    uint8_t timestamp[12];
    uint8_t local_static_public[32];

    memset(message, 0, WG_MSG_INITIATION_SIZE);

    state->active = true;
    state->initiator = true;
    state->local_index = local_index;
    wg_noise_reset(state, remote_static_public);

    message[0] = WG_MSG_INITIATION;
    put_le32(message + 4, local_index);

    if (wg_random(state->ephemeral_private, 32) < 0)
	return -1;
    if (wg_x25519_public(state->ephemeral_public,
			 state->ephemeral_private) < 0)
	return -1;
    memcpy(message + 8, state->ephemeral_public, 32);

    /* Ci = KDF1(Ci, Epub_i); Hi = HASH(Hi || Epub_i) */
    wg_kdf(out, 1, state->chaining_key, state->ephemeral_public, 32);
    memcpy(state->chaining_key, out[0], 32);
    hash_concat(state->hash, state->hash, 32, state->ephemeral_public, 32);

    /* msg.static = AEAD(KDF2(Ci, DH(Epriv_i, Spub_r)), Spub_i, Hi) */
    if (wg_x25519(dh, state->ephemeral_private, remote_static_public) < 0)
	return -1;
    wg_kdf(out, 2, state->chaining_key, dh, 32);
    memcpy(state->chaining_key, out[0], 32);
    if (wg_x25519_public(local_static_public, local_static_private) < 0)
	return -1;
    if (wg_aead_encrypt(message + 40, out[1], 0, local_static_public, 32,
			state->hash, 32) != 48)
	return -1;
    hash_concat(state->hash, state->hash, 32, message + 40, 48);

    /* msg.timestamp = AEAD(KDF2(Ci, DH(Spriv_i, Spub_r)), TAI64N, Hi) */
    if (wg_x25519(dh, local_static_private, remote_static_public) < 0)
	return -1;
    wg_kdf(out, 2, state->chaining_key, dh, 32);
    memcpy(state->chaining_key, out[0], 32);
    wg_tai64n(timestamp);
    if (wg_aead_encrypt(message + 88, out[1], 0, timestamp, 12,
			state->hash, 32) != 28)
	return -1;
    hash_concat(state->hash, state->hash, 32, message + 88, 28);

    wg_noise_mac1(message + 116, remote_static_public, message, 116);
    return 0;
}

int wg_noise_consume_response(WgNoise *state,
			      const uint8_t local_static_private[32],
			      const uint8_t preshared_key[32],
			      bool has_preshared_key,
			      const uint8_t message[WG_MSG_RESPONSE_SIZE],
			      uint8_t send_key[32], uint8_t recv_key[32])
{
    static const uint8_t zero_key[32] = { 0 };
    uint8_t out[3][32];
    uint8_t dh[32];
    uint8_t empty[16];

    if (!state->active || !state->initiator)
	return -1;
    if (message[0] != WG_MSG_RESPONSE)
	return -1;
    if (get_le32(message + 8) != state->local_index)
	return -1;

    /* Ci = KDF1(Ci, Epub_r); Hi = HASH(Hi || Epub_r) */
    wg_kdf(out, 1, state->chaining_key, message + 12, 32);
    memcpy(state->chaining_key, out[0], 32);
    hash_concat(state->hash, state->hash, 32, message + 12, 32);

    /* Ci = KDF1(Ci, DH(Epriv_i, Epub_r)) */
    if (wg_x25519(dh, state->ephemeral_private, message + 12) < 0)
	return -1;
    wg_kdf(out, 1, state->chaining_key, dh, 32);
    memcpy(state->chaining_key, out[0], 32);

    /* Ci = KDF1(Ci, DH(Spriv_i, Epub_r)) */
    if (wg_x25519(dh, local_static_private, message + 12) < 0)
	return -1;
    wg_kdf(out, 1, state->chaining_key, dh, 32);
    memcpy(state->chaining_key, out[0], 32);

    /* (Ci, tau, k) = KDF3(Ci, Q); Hi = HASH(Hi || tau) */
    wg_kdf(out, 3, state->chaining_key,
	   has_preshared_key ? preshared_key : zero_key, 32);
    memcpy(state->chaining_key, out[0], 32);
    hash_concat(state->hash, state->hash, 32, out[1], 32);

    /* verify AEAD(k, 0, empty, Hi) */
    if (wg_aead_decrypt(empty, out[2], 0, message + 44, 16,
			state->hash, 32) != 0)
	return -1;
    hash_concat(state->hash, state->hash, 32, message + 44, 16);

    /* (Tsend_i, Trecv_i) = KDF2(Ci, empty) */
    wg_kdf(out, 2, state->chaining_key, NULL, 0);
    memcpy(send_key, out[0], 32);
    memcpy(recv_key, out[1], 32);

    state->remote_index = get_le32(message + 4);
    return 0;
}

int wg_noise_consume_initiation(WgNoise *state,
				const uint8_t local_static_private[32],
				const uint8_t local_static_public[32],
				const uint8_t preshared_key[32],
				bool has_preshared_key,
				const uint8_t message[WG_MSG_INITIATION_SIZE],
				uint8_t response[WG_MSG_RESPONSE_SIZE],
				uint32_t local_index, uint32_t *remote_index,
				uint8_t remote_static_public[32],
				uint8_t send_key[32], uint8_t recv_key[32])
{
    static const uint8_t zero_key[32] = { 0 };
    uint8_t out[3][32];
    uint8_t dh[32];
    uint8_t key[32];
    uint8_t timestamp[12];
    uint8_t initiator_static[32];
    uint8_t expected_mac1[16];

    if (message[0] != WG_MSG_INITIATION)
	return -1;

    state->active = true;
    state->initiator = false;
    state->local_index = local_index;
    state->remote_index = 0;

    /* Hr = HASH(INITIAL_HASH || Spub_r); the peer arg is our static.
     * wg_noise_reset() leaves the replay window alone.  */
    wg_noise_reset(state, local_static_public);

    /* Cr = KDF1(Cr, Epub_i); Hr = HASH(Hr || Epub_i) */
    wg_kdf(out, 1, state->chaining_key, message + 8, 32);
    memcpy(state->chaining_key, out[0], 32);
    hash_concat(state->hash, state->hash, 32, message + 8, 32);

    /* (Cr, k) = KDF2(Cr, DH(Spriv_r, Epub_i)); decrypt the static.  */
    if (wg_x25519(dh, local_static_private, message + 8) < 0)
	return -1;
    wg_kdf(out, 2, state->chaining_key, dh, 32);
    memcpy(state->chaining_key, out[0], 32);
    if (wg_aead_decrypt(initiator_static, out[1], 0, message + 40, 48,
			state->hash, 32) != 32)
	return -1;
    hash_concat(state->hash, state->hash, 32, message + 40, 48);
    memcpy(remote_static_public, initiator_static, 32);

    /* (Cr, k) = KDF2(Cr, DH(Spriv_r, Spub_i)); decrypt the timestamp.  */
    if (wg_x25519(dh, local_static_private, initiator_static) < 0)
	return -1;
    wg_kdf(out, 2, state->chaining_key, dh, 32);
    memcpy(state->chaining_key, out[0], 32);
    if (wg_aead_decrypt(timestamp, out[1], 0, message + 88, 28,
			state->hash, 32) != 12)
	return -1;
    hash_concat(state->hash, state->hash, 32, message + 88, 28);

    if (!wg_noise_timestamp_ok(state, timestamp))
	return -1;

    /* The first authentication tag is mandatory.  */
    wg_noise_mac1(expected_mac1, local_static_public, message, 116);
    if (memcmp(expected_mac1, message + 116, 16) != 0)
	return -1;

    state->remote_index = get_le32(message + 4);
    *remote_index = state->remote_index;

    if (wg_random(state->ephemeral_private, 32) < 0)
	return -1;
    if (wg_x25519_public(state->ephemeral_public,
			 state->ephemeral_private) < 0)
	return -1;

    memset(response, 0, WG_MSG_RESPONSE_SIZE);
    response[0] = WG_MSG_RESPONSE;
    put_le32(response + 4, local_index);
    put_le32(response + 8, state->remote_index);
    memcpy(response + 12, state->ephemeral_public, 32);

    /* Cr = KDF1(Cr, Epub_r); Hr = HASH(Hr || Epub_r) */
    wg_kdf(out, 1, state->chaining_key, state->ephemeral_public, 32);
    memcpy(state->chaining_key, out[0], 32);
    hash_concat(state->hash, state->hash, 32, state->ephemeral_public, 32);

    /* Cr = KDF1(Cr, DH(Epriv_r, Epub_i)) */
    if (wg_x25519(dh, state->ephemeral_private, message + 8) < 0)
	return -1;
    wg_kdf(out, 1, state->chaining_key, dh, 32);
    memcpy(state->chaining_key, out[0], 32);

    /* Cr = KDF1(Cr, DH(Epriv_r, Spub_i)) */
    if (wg_x25519(dh, state->ephemeral_private, initiator_static) < 0)
	return -1;
    wg_kdf(out, 1, state->chaining_key, dh, 32);
    memcpy(state->chaining_key, out[0], 32);

    /* (Cr, tau, k) = KDF3(Cr, Q) */
    wg_kdf(out, 3, state->chaining_key,
	   has_preshared_key ? preshared_key : zero_key, 32);
    memcpy(state->chaining_key, out[0], 32);
    memcpy(key, out[2], 32);
    hash_concat(state->hash, state->hash, 32, out[1], 32);

    /* msg.empty = AEAD(k, 0, empty, Hr) */
    if (wg_aead_encrypt(response + 44, key, 0, NULL, 0,
			state->hash, 32) != 16)
	return -1;
    hash_concat(state->hash, state->hash, 32, response + 44, 16);

    /* (Tsend_r, Trecv_r) = KDF2(Cr, empty) */
    wg_kdf(out, 2, state->chaining_key, NULL, 0);
    memcpy(recv_key, out[0], 32);
    memcpy(send_key, out[1], 32);

    /* mac1 of the response is keyed by the initiator's static.  */
    wg_noise_mac1(response + 60, initiator_static, response, 60);
    return 0;
}
