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

#ifndef NETVIRT_WG_NOISE_H
#define NETVIRT_WG_NOISE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* WireGuard message types.  */
#define WG_MSG_INITIATION	1
#define WG_MSG_RESPONSE		2
#define WG_MSG_COOKIE		3
#define WG_MSG_DATA		4

#define WG_MSG_INITIATION_SIZE	148
#define WG_MSG_RESPONSE_SIZE	92
#define WG_MSG_COOKIE_SIZE	64
#define WG_DATA_HEADER_SIZE	16

/* The Noise_IKpsk2 handshake state, from the WireGuard whitepaper.  */
typedef struct WgNoise {
    bool active;
    bool initiator;
    uint32_t local_index;
    uint32_t remote_index;
    uint8_t chaining_key[32];
    uint8_t hash[32];
    uint8_t ephemeral_private[32];
    uint8_t ephemeral_public[32];
    uint8_t last_timestamp[12];	/* responder: replay protection */
} WgNoise;

/* Reset @state to the initial chaining key and hash, with the peer's
 * static public key mixed in.  */
extern void wg_noise_reset(WgNoise *state, const uint8_t peer_static[32]);

/* Initiator: build a handshake initiation (148 bytes) and keep the
 * state needed to consume the response.  */
extern int wg_noise_create_initiation(WgNoise *state, uint32_t local_index,
				      const uint8_t local_static_private[32],
				      const uint8_t remote_static_public[32],
				      uint8_t message[WG_MSG_INITIATION_SIZE]);

/* Initiator: consume the response (92 bytes) and derive the transport
 * keys.  @send_key is what this side encrypts with.  */
extern int wg_noise_consume_response(WgNoise *state,
				     const uint8_t local_static_private[32],
				     const uint8_t preshared_key[32],
				     bool has_preshared_key,
				     const uint8_t message[WG_MSG_RESPONSE_SIZE],
				     uint8_t send_key[32],
				     uint8_t recv_key[32]);

/* Responder: consume an initiation, build the response and derive the
 * transport keys.  @remote_static_public receives the initiator's
 * static public key.  */
extern int wg_noise_consume_initiation(WgNoise *state,
				       const uint8_t local_static_private[32],
				       const uint8_t local_static_public[32],
				       const uint8_t preshared_key[32],
				       bool has_preshared_key,
				       const uint8_t message[WG_MSG_INITIATION_SIZE],
				       uint8_t response[WG_MSG_RESPONSE_SIZE],
				       uint32_t local_index,
				       uint32_t *remote_index,
				       uint8_t remote_static_public[32],
				       uint8_t send_key[32],
				       uint8_t recv_key[32]);

/* mac1 = BLAKE2s-128(HASH("mac1----" || peer_static), message_up_to_mac1).
 * @length is the offset of mac1 inside @message.  */
extern void wg_noise_mac1(uint8_t mac1[16], const uint8_t peer_static[32],
			  const uint8_t *message, size_t length);

/* Replay protection for handshake timestamps (TAI64N is monotonic).  */
extern bool wg_noise_timestamp_ok(WgNoise *state, const uint8_t timestamp[12]);

#endif				/* NETVIRT_WG_NOISE_H */
