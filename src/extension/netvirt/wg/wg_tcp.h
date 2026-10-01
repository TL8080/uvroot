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

#ifndef NETVIRT_WG_TCP_H
#define NETVIRT_WG_TCP_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A small user-space TCP terminator for the WireGuard bridge.  The
 * container's own sockets live in the host network namespace, so a
 * connection arriving through the tunnel cannot reach them: this module
 * terminates the TCP flow itself and relays the byte stream to a local
 * socket (for instance a service listening on 127.0.0.1).
 *
 * Only IPv4 and the server side are implemented; options other than MSS
 * are ignored, the receive window is fixed, and a simple go-back-N
 * timer handles retransmission.  It speaks to the Linux client on the
 * other side of a reliable tunnel.
 */

typedef struct WgTcp WgTcp;

/* Called to send an IPv4 packet back through the tunnel.  */
typedef int (*WgTcpSend)(void *opaque, const uint8_t *packet, size_t length);

extern WgTcp *wg_tcp_new(WgTcpSend send, void *opaque);

extern void wg_tcp_free(WgTcp *tcp);

/* Terminate @local_ip:@local_port (network order / host order) and relay
 * the stream to @host_ip:@host_port.  Returns 0 on success.  */
extern int wg_tcp_add_forward(WgTcp *tcp, uint32_t local_ip,
			      uint16_t local_port, uint32_t host_ip,
			      uint16_t host_port);

/* Feed an IPv4 packet coming from the tunnel: 1 when it belonged to a
 * forwarded flow (and was consumed), 0 otherwise.  */
extern int wg_tcp_input(WgTcp *tcp, const uint8_t *packet, size_t length);

/* Poll integration: append the backend descriptors to @fds (@max
 * entries), then report their readiness after poll(2).  */
extern int wg_tcp_pollfds(WgTcp *tcp, struct pollfd *fds, int max);
extern void wg_tcp_poll(WgTcp *tcp, struct pollfd *fds, int count);

/* Milliseconds until the next retransmission, or -1.  */
extern int wg_tcp_timeout(WgTcp *tcp);
extern void wg_tcp_tick(WgTcp *tcp);

extern int wg_tcp_connection_count(WgTcp *tcp);

#endif				/* NETVIRT_WG_TCP_H */
