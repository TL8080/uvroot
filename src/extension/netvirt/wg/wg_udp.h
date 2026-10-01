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

#ifndef NETVIRT_WG_UDP_H
#define NETVIRT_WG_UDP_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Datagram forwarding for the WireGuard bridge: a UDP service of the
 * container (a DNS resolver on port 53, typically) is not reachable
 * through the tunnel either, since the container's sockets live in the
 * host network namespace.  Each datagram arriving for a forwarded
 * address is sent to a local socket from a per-client socket, and the
 * answer is wrapped back into a UDP/IP packet for the tunnel.
 */

typedef struct WgUdp WgUdp;

typedef int (*WgUdpSend)(void *opaque, const uint8_t *packet, size_t length);

extern WgUdp *wg_udp_new(WgUdpSend send, void *opaque);
extern void wg_udp_free(WgUdp *forward);

extern int wg_udp_add_forward(WgUdp *forward, uint32_t local_ip,
			      uint16_t local_port, uint32_t host_ip,
			      uint16_t host_port);

/* 1 when the packet belonged to a forwarded flow.  */
extern int wg_udp_input(WgUdp *forward, const uint8_t *packet, size_t length);

extern int wg_udp_pollfds(WgUdp *forward, struct pollfd *fds, int max);
extern void wg_udp_poll(WgUdp *forward, struct pollfd *fds, int count);
extern void wg_udp_tick(WgUdp *forward);

#endif				/* NETVIRT_WG_UDP_H */
