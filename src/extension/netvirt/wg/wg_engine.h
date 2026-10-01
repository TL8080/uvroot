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

#ifndef NETVIRT_WG_ENGINE_H
#define NETVIRT_WG_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WG_MAX_PACKET	65535
#define WG_MAX_PEERS	16
#define WG_MAX_FORWARDS	8
#define WG_MAX_ALLOWED	16
#define WG_QUEUED_PACKETS 8

typedef struct WgAllowedIp {
    int family;			/* AF_INET or AF_INET6 */
    uint8_t address[16];
    uint8_t prefix;
} WgAllowedIp;

typedef struct WgPeerConfig {
    uint8_t public_key[32];
    bool has_preshared_key;
    uint8_t preshared_key[32];
    char endpoint[256];		/* "host:port" or "" */
    WgAllowedIp allowed[WG_MAX_ALLOWED];
    int allowed_count;
    int keepalive;
    struct WgPeerConfig *next;
} WgPeerConfig;

/*
 * A local TCP service exposed through the tunnel: connections to
 * @local_ip:@local_port are terminated and relayed to
 * @host_ip:@host_port.  Addresses are in network order, ports in host
 * order.
 */
typedef struct WgForwardConfig {
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t host_ip;
    uint16_t host_port;
} WgForwardConfig;

typedef struct WgEngineConfig {
    uint8_t private_key[32];
    uint8_t public_key[32];	/* derived from private_key */
    uint16_t listen_port;
    WgPeerConfig *peers;
    WgForwardConfig forwards[WG_MAX_FORWARDS];
    int forward_count;
    WgForwardConfig udp_forwards[WG_MAX_FORWARDS];
    int udp_forward_count;
} WgEngineConfig;

typedef struct WgEngine WgEngine;

/* Called for every successfully decrypted IP packet.  */
typedef void (*WgPacketCallback)(void *opaque, const uint8_t *packet,
				 size_t length);

typedef void (*WgLogCallback)(void *opaque, int level, const char *message);

/* Parse a wg(8)/wg-quick body or a flat "key=value;..." string.  Returns
 * 0 on success, -1 with @error filled otherwise.  */
extern int wg_engine_config_parse(WgEngineConfig *config, const char *text,
				  char *error, size_t error_size);
extern void wg_engine_config_free(WgEngineConfig *config);

/*
 * Create an engine around @tun_fd.  When @tun_fd is >= 0 the engine
 * reads IP packets from it and writes the decrypted ones back; when it
 * is -1 the caller injects packets with wg_engine_send_ip() and receives
 * them through @callback (that is what the interop test uses).
 */
extern WgEngine *wg_engine_new(const WgEngineConfig *config, int tun_fd,
			       WgPacketCallback callback, void *opaque,
			       char *error, size_t error_size);
extern void wg_engine_free(WgEngine *engine);

/* Run one poll iteration, processing timers and at most one round of
 * I/O.  Returns the number of events handled.  */
extern int wg_engine_run_once(WgEngine *engine, int timeout_ms);

/* Same, in a background thread.  */
extern int wg_engine_start(WgEngine *engine);
extern void wg_engine_stop(WgEngine *engine);

/* Encrypt @packet and send it to the peer whose allowed-ips match it
 * (falling back to the first peer).  Starts a handshake when no session
 * exists yet; the packet is queued until the session is up.  */
extern int wg_engine_send_ip(WgEngine *engine, const uint8_t *packet,
			     size_t length);

/* Start a handshake with the first peer now.  */
extern void wg_engine_handshake(WgEngine *engine);

/* True once at least one transport session is established.  */
extern bool wg_engine_ready(WgEngine *engine);

extern void wg_engine_set_log(WgEngine *engine, WgLogCallback log,
			      void *opaque);

#endif				/* NETVIRT_WG_ENGINE_H */
