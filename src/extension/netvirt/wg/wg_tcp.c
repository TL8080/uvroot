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

#include "extension/netvirt/wg/wg_tcp.h"
#include "extension/netvirt/wg/wg_crypto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define TCP_MSS			1380
#define TCP_WINDOW		65535
#define TCP_BUFFER		(256 * 1024)
#define TCP_RETRANSMIT_MS	200
#define TCP_IDLE_MS		120000
#define TCP_MAX_CONNECTIONS	16
#define TCP_MAX_FORWARDS	8

#define TCP_FIN	0x01
#define TCP_SYN	0x02
#define TCP_RST	0x04
#define TCP_PSH	0x08
#define TCP_ACK	0x10

enum tcp_state {
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
};

typedef struct WgForward {
    uint32_t local_ip;		/* network order */
    uint16_t local_port;	/* host order */
    uint32_t host_ip;		/* network order */
    uint16_t host_port;		/* host order */
} WgForward;

typedef struct TcpConn {
    struct TcpConn *next;
    uint32_t remote_ip;		/* network order */
    uint16_t remote_port;	/* host order */
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t host_ip;
    uint16_t host_port;
    int state;
    int backend_fd;

    uint32_t iss;
    uint32_t snd_una;		/* oldest unacknowledged sequence number */
    uint32_t snd_nxt;		/* next sequence number to send */
    uint32_t snd_wnd;		/* peer's advertised window */
    uint32_t rcv_nxt;
    uint16_t peer_mss;
    bool fin_sent;
    bool fin_acked;
    bool peer_fin;
    bool backend_eof;

    uint8_t *snd_buf;		/* from snd_una to snd_una + snd_len */
    size_t snd_len;

    uint8_t *in_buf;		/* received from the tunnel, not written yet */
    size_t in_len;
    size_t in_cap;

    uint64_t last_activity_ms;
    uint64_t retransmit_ms;
    bool retransmit_pending;
} TcpConn;

struct WgTcp {
    WgTcpSend send;
    void *opaque;
    WgForward forwards[TCP_MAX_FORWARDS];
    int forward_count;
    TcpConn *connections;
    int connection_count;
    TcpConn *poll_connections[TCP_MAX_CONNECTIONS];
};

/* ------------------------------------------------------------------ */
/* Byte order and checksums                                            */
/* ------------------------------------------------------------------ */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t) ((p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16)
	| ((uint32_t) p[2] << 8) | p[3];
}

static void wr16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t) (value >> 8);
    p[1] = (uint8_t) value;
}

static void wr32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t) (value >> 24);
    p[1] = (uint8_t) (value >> 16);
    p[2] = (uint8_t) (value >> 8);
    p[3] = (uint8_t) value;
}

static uint64_t now_ms(void)
{
    struct timespec time;

    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t) time.tv_sec * 1000 + (uint64_t) time.tv_nsec / 1000000;
}

static uint32_t checksum_add(uint32_t sum, const uint8_t *data, size_t length)
{
    while (length > 1) {
	sum += ((uint32_t) data[0] << 8) | data[1];
	data += 2;
	length -= 2;
    }
    if (length > 0)
	sum += (uint32_t) data[0] << 8;
    return sum;
}

static uint16_t checksum_finish(uint32_t sum)
{
    while (sum >> 16)
	sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t) ~sum;
}

/* Signed 32-bit sequence comparison.  */
static bool sequence_after(uint32_t a, uint32_t b)
{
    return (int32_t) (a - b) > 0;
}

/* ------------------------------------------------------------------ */
/* Connections                                                         */
/* ------------------------------------------------------------------ */

static void conn_close(WgTcp *tcp, TcpConn *connection)
{
    TcpConn **link;

    for (link = &tcp->connections; *link != NULL; link = &(*link)->next) {
	if (*link == connection) {
	    *link = connection->next;
	    break;
	}
    }
    if (connection->backend_fd >= 0)
	close(connection->backend_fd);
    free(connection->snd_buf);
    free(connection->in_buf);
    free(connection);
    tcp->connection_count--;
}

static TcpConn *conn_find(WgTcp *tcp, uint32_t remote_ip,
			  uint16_t remote_port, uint32_t local_ip,
			  uint16_t local_port)
{
    TcpConn *connection;

    for (connection = tcp->connections; connection != NULL;
	 connection = connection->next) {
	if (connection->remote_ip == remote_ip
	    && connection->remote_port == remote_port
	    && connection->local_ip == local_ip
	    && connection->local_port == local_port)
	    return connection;
    }
    return NULL;
}

static TcpConn *conn_create(WgTcp *tcp, const WgForward *forward,
			    uint32_t remote_ip, uint16_t remote_port)
{
    TcpConn *connection;
    uint8_t random[4];

    if (tcp->connection_count >= TCP_MAX_CONNECTIONS)
	return NULL;
    connection = calloc(1, sizeof(*connection));
    if (connection == NULL)
	return NULL;
    connection->snd_buf = malloc(TCP_BUFFER);
    connection->in_buf = malloc(TCP_BUFFER);
    if (connection->snd_buf == NULL || connection->in_buf == NULL) {
	free(connection->snd_buf);
	free(connection->in_buf);
	free(connection);
	return NULL;
    }
    connection->in_cap = TCP_BUFFER;
    connection->backend_fd = -1;
    connection->remote_ip = remote_ip;
    connection->remote_port = remote_port;
    connection->local_ip = forward->local_ip;
    connection->local_port = forward->local_port;
    connection->host_ip = forward->host_ip;
    connection->host_port = forward->host_port;
    connection->peer_mss = TCP_MSS;
    connection->snd_wnd = TCP_WINDOW;
    connection->last_activity_ms = now_ms();
    if (wg_random(random, sizeof(random)) == 0)
	connection->iss = rd32(random);
    else
	connection->iss = (uint32_t) now_ms();
    connection->snd_una = connection->iss;
    connection->snd_nxt = connection->iss + 1;	/* SYN consumes one */
    connection->state = TCP_SYN_RECEIVED;
    connection->next = tcp->connections;
    tcp->connections = connection;
    tcp->connection_count++;
    return connection;
}

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

static uint16_t receive_window(const TcpConn *connection)
{
    size_t free_space = connection->in_cap - connection->in_len;

    return (free_space > TCP_WINDOW) ? TCP_WINDOW : (uint16_t) free_space;
}

static int tcp_output(WgTcp *tcp, TcpConn *connection, uint8_t flags,
		      uint32_t sequence, uint32_t acknowledgement,
		      const uint8_t *payload, size_t payload_length,
		      bool with_mss)
{
    uint8_t packet[2048];
    uint8_t *header;
    size_t header_length = with_mss ? 24 : 20;
    size_t total = 20 + header_length + payload_length;
    uint32_t sum;

    if (total > sizeof(packet))
	return -1;
    memset(packet, 0, 20 + header_length);

    packet[0] = 0x45;
    wr16(packet + 2, (uint16_t) total);
    packet[8] = 64;
    packet[9] = 6;		/* TCP */
    wr32(packet + 12, connection->local_ip);
    wr32(packet + 16, connection->remote_ip);
    wr16(packet + 10, checksum_finish(checksum_add(0, packet, 20)));

    header = packet + 20;
    wr16(header + 0, connection->local_port);
    wr16(header + 2, connection->remote_port);
    wr32(header + 4, sequence);
    wr32(header + 8, acknowledgement);
    header[12] = (uint8_t) ((header_length / 4) << 4);
    header[13] = flags;
    wr16(header + 14, receive_window(connection));
    if (with_mss) {
	header[20] = 2;		/* MSS option */
	header[21] = 4;
	wr16(header + 22, TCP_MSS);
    }
    if (payload_length > 0)
	memcpy(packet + 20 + header_length, payload, payload_length);

    sum = checksum_add(0, packet + 12, 8);	/* source and destination */
    sum += 6;					/* zero + protocol */
    sum += (uint16_t) (header_length + payload_length);
    sum = checksum_add(sum, header, header_length + payload_length);
    wr16(header + 16, checksum_finish(sum));

    return tcp->send(tcp->opaque, packet, total);
}

static void send_pending(WgTcp *tcp, TcpConn *connection)
{
    while (connection->snd_len > 0) {
	size_t offset, available, chunk, window;

	if (connection->snd_nxt < connection->snd_una)
	    break;
	offset = (size_t) (connection->snd_nxt - connection->snd_una);
	if (offset >= connection->snd_len)
	    break;
	available = connection->snd_len - offset;
	window = connection->snd_wnd
	    - (connection->snd_nxt - connection->snd_una);
	if ((int32_t) window <= 0)
	    break;
	chunk = available < connection->peer_mss ? available
	    : connection->peer_mss;
	if (chunk > window)
	    chunk = window;
	if (chunk == 0)
	    break;
	tcp_output(tcp, connection, TCP_PSH | TCP_ACK, connection->snd_nxt,
		   connection->rcv_nxt, connection->snd_buf + offset, chunk,
		   false);
	connection->snd_nxt += (uint32_t) chunk;
	connection->last_activity_ms = now_ms();
	connection->retransmit_pending = true;
	connection->retransmit_ms = connection->last_activity_ms
	    + TCP_RETRANSMIT_MS;
    }
}

static void send_fin_if_done(WgTcp *tcp, TcpConn *connection)
{
    if (connection->backend_eof && !connection->fin_sent
	&& connection->snd_nxt == connection->snd_una + connection->snd_len) {
	connection->fin_sent = true;
	connection->state = (connection->state == TCP_ESTABLISHED)
	    ? TCP_FIN_WAIT_1 : connection->state;
	tcp_output(tcp, connection, TCP_FIN | TCP_ACK,
		   connection->snd_nxt, connection->rcv_nxt, NULL, 0, false);
	connection->snd_nxt += 1;
	connection->retransmit_pending = true;
	connection->retransmit_ms = now_ms() + TCP_RETRANSMIT_MS;
    }
}

/* ------------------------------------------------------------------ */
/* Backend socket                                                      */
/* ------------------------------------------------------------------ */

static void backend_connect(TcpConn *connection)
{
    struct sockaddr_in address;
    int fd;
    int status;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
	return;
    (void) fcntl(fd, F_SETFL, O_NONBLOCK);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(connection->host_ip);
    address.sin_port = htons(connection->host_port);
    status = connect(fd, (struct sockaddr *) &address, sizeof(address));
    if (status < 0 && errno != EINPROGRESS) {
	close(fd);
	return;
    }
    connection->backend_fd = fd;
}

static void backend_flush(TcpConn *connection)
{
    while (connection->in_len > 0 && connection->backend_fd >= 0) {
	ssize_t written = write(connection->backend_fd, connection->in_buf,
				connection->in_len);

	if (written > 0) {
	    memmove(connection->in_buf, connection->in_buf + written,
		    connection->in_len - (size_t) written);
	    connection->in_len -= (size_t) written;
	    continue;
	}
	if (written < 0 && errno == EINTR)
	    continue;
	break;
    }
}

static void backend_read(WgTcp *tcp, TcpConn *connection)
{
    while (connection->snd_len < TCP_BUFFER) {
	size_t space = TCP_BUFFER - connection->snd_len;
	ssize_t length = read(connection->backend_fd,
			      connection->snd_buf + connection->snd_len,
			      space < 32768 ? space : 32768);

	if (length > 0) {
	    connection->snd_len += (size_t) length;
	    connection->last_activity_ms = now_ms();
	    continue;
	}
	if (length == 0)
	    connection->backend_eof = true;
	else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
	    connection->backend_eof = true;
	break;
    }
    send_pending(tcp, connection);
    send_fin_if_done(tcp, connection);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void handle_acknowledgement(TcpConn *connection, uint32_t ack)
{
    size_t acknowledged;

    if (!sequence_after(ack, connection->snd_una))
	return;
    acknowledged = (size_t) (ack - connection->snd_una);
    if (acknowledged <= connection->snd_len) {
	memmove(connection->snd_buf, connection->snd_buf + acknowledged,
		connection->snd_len - acknowledged);
	connection->snd_len -= acknowledged;
    } else
	connection->snd_len = 0;
    connection->snd_una = ack;
    if (sequence_after(connection->snd_una, connection->snd_nxt))
	connection->snd_nxt = connection->snd_una;
    if (connection->fin_sent && !connection->fin_acked
	&& !sequence_after(connection->snd_nxt, ack))
	connection->fin_acked = true;
    if (!sequence_after(connection->snd_nxt, connection->snd_una))
	connection->retransmit_pending = false;
    connection->last_activity_ms = now_ms();
}

static void conn_input(WgTcp *tcp, TcpConn *connection, const uint8_t *packet,
		       size_t length, size_t ip_header_length)
{
    const uint8_t *header = packet + ip_header_length;
    size_t tcp_header_length;
    size_t payload_length;
    const uint8_t *payload;
    uint32_t sequence, acknowledgement;
    uint8_t flags;
    uint16_t window;

    if (length < ip_header_length + 20)
	return;
    tcp_header_length = (size_t) (header[12] >> 4) * 4;
    if (tcp_header_length < 20 || length < ip_header_length + tcp_header_length)
	return;
    payload = header + tcp_header_length;
    payload_length = length - ip_header_length - tcp_header_length;
    sequence = rd32(header + 4);
    acknowledgement = rd32(header + 8);
    flags = header[13];
    window = rd16(header + 14);

    if (flags & TCP_RST) {
	conn_close(tcp, connection);
	return;
    }

    if (flags & TCP_SYN) {
	/* A retransmitted SYN: repeat the SYN-ACK.  */
	if (connection->state == TCP_SYN_RECEIVED)
	    tcp_output(tcp, connection, TCP_SYN | TCP_ACK, connection->iss,
		       connection->rcv_nxt, NULL, 0, true);
	return;
    }

    if (flags & TCP_ACK)
	handle_acknowledgement(connection, acknowledgement);

    if (connection->state == TCP_SYN_RECEIVED
	&& (flags & TCP_ACK) && acknowledgement == connection->snd_nxt) {
	connection->snd_wnd = window;
	connection->state = TCP_ESTABLISHED;
	backend_connect(connection);
    }

    if (payload_length > 0) {
	if (sequence == connection->rcv_nxt) {
	    size_t room = connection->in_cap - connection->in_len;

	    if (payload_length <= room) {
		memcpy(connection->in_buf + connection->in_len, payload,
		       payload_length);
		connection->in_len += payload_length;
	    }
	    connection->rcv_nxt += (uint32_t) payload_length;
	    tcp_output(tcp, connection, TCP_ACK, connection->snd_nxt,
		       connection->rcv_nxt, NULL, 0, false);
	    backend_flush(connection);
	} else
	    /* Duplicate or out of order: acknowledge what we have.  */
	    tcp_output(tcp, connection, TCP_ACK, connection->snd_nxt,
		       connection->rcv_nxt, NULL, 0, false);
	connection->last_activity_ms = now_ms();
    }

    if (flags & TCP_FIN) {
	connection->rcv_nxt += 1;
	connection->peer_fin = true;
	tcp_output(tcp, connection, TCP_ACK, connection->snd_nxt,
		   connection->rcv_nxt, NULL, 0, false);
	if (connection->state == TCP_ESTABLISHED)
	    connection->state = TCP_CLOSE_WAIT;
	else if (connection->state == TCP_FIN_WAIT_2)
	    connection->state = TCP_TIME_WAIT;
    }

    send_pending(tcp, connection);
    send_fin_if_done(tcp, connection);

    if (connection->fin_acked && connection->peer_fin
	&& connection->snd_len == 0) {
	conn_close(tcp, connection);
	return;
    }
}

static const WgForward *forward_for(WgTcp *tcp, uint32_t local_ip,
				    uint16_t local_port)
{
    int index;

    for (index = 0; index < tcp->forward_count; index++) {
	if (tcp->forwards[index].local_ip == local_ip
	    && tcp->forwards[index].local_port == local_port)
	    return &tcp->forwards[index];
    }
    return NULL;
}

int wg_tcp_input(WgTcp *tcp, const uint8_t *packet, size_t length)
{
    size_t ip_header_length;
    uint32_t source, destination;
    const uint8_t *header;
    uint16_t source_port, destination_port;
    const WgForward *forward;
    TcpConn *connection;

    if (length < 20 || (packet[0] >> 4) != 4 || packet[9] != 6)
	return 0;
    ip_header_length = (size_t) (packet[0] & 0x0F) * 4;
    if (ip_header_length < 20 || length < ip_header_length + 20)
	return 0;
    source = rd32(packet + 12);
    destination = rd32(packet + 16);
    header = packet + ip_header_length;
    source_port = rd16(header + 0);
    destination_port = rd16(header + 2);

    connection = conn_find(tcp, source, source_port, destination,
			   destination_port);
    if (connection != NULL) {
	conn_input(tcp, connection, packet, length, ip_header_length);
	return 1;
    }

    forward = forward_for(tcp, destination, destination_port);
    if (forward == NULL)
	return 0;

    if (header[13] & TCP_SYN) {
	connection = conn_create(tcp, forward, source, source_port);
	if (connection == NULL)
	    return 1;
	connection->rcv_nxt = rd32(header + 4) + 1;
	tcp_output(tcp, connection, TCP_SYN | TCP_ACK, connection->iss,
		   connection->rcv_nxt, NULL, 0, true);
	connection->retransmit_pending = true;
	connection->retransmit_ms = now_ms() + TCP_RETRANSMIT_MS;
	return 1;
    }

    /* Nothing is listening for this flow: refuse it.  */
    {
	uint8_t reset[40];
	uint32_t sum;

	memset(reset, 0, sizeof(reset));
	reset[0] = 0x45;
	wr16(reset + 2, sizeof(reset));
	reset[8] = 64;
	reset[9] = 6;
	wr32(reset + 12, destination);
	wr32(reset + 16, source);
	wr16(reset + 10, checksum_finish(checksum_add(0, reset, 20)));
	wr16(reset + 20, destination_port);
	wr16(reset + 22, source_port);
	wr32(reset + 24, 0);
	wr32(reset + 28, rd32(header + 4) + ((header[13] & TCP_SYN) ? 1 : 0)
	     + (uint32_t) (length - ip_header_length));
	reset[32] = 0x50;
	reset[33] = TCP_RST | TCP_ACK;
	sum = checksum_add(0, reset + 12, 8) + 6 + 20;
	sum = checksum_add(sum, reset + 20, 20);
	wr16(reset + 36, checksum_finish(sum));
	(void) tcp->send(tcp->opaque, reset, sizeof(reset));
    }
    return 1;
}

void wg_tcp_tick(WgTcp *tcp)
{
    TcpConn *connection;
    TcpConn *next;
    uint64_t now = now_ms();

    for (connection = tcp->connections; connection != NULL;
	 connection = next) {
	next = connection->next;

	if (connection->retransmit_pending
	    && now >= connection->retransmit_ms) {
	    connection->retransmit_pending = false;
	    if (connection->state == TCP_SYN_RECEIVED)
		tcp_output(tcp, connection, TCP_SYN | TCP_ACK,
			   connection->iss, connection->rcv_nxt, NULL, 0,
			   true);
	    else {
		/* Go-back-N: resend what has not been acknowledged.  */
		size_t offset = 0;

		while (connection->snd_una + offset < connection->snd_nxt) {
		    size_t remaining =
			(size_t) (connection->snd_nxt - connection->snd_una)
			- offset;
		    size_t chunk = remaining < connection->peer_mss
			? remaining : connection->peer_mss;

		    tcp_output(tcp, connection, TCP_PSH | TCP_ACK,
			       connection->snd_una + (uint32_t) offset,
			       connection->rcv_nxt,
			       connection->snd_buf + offset, chunk, false);
		    offset += chunk;
		}
		if (connection->fin_sent && !connection->fin_acked)
		    tcp_output(tcp, connection, TCP_FIN | TCP_ACK,
			       connection->snd_nxt - 1, connection->rcv_nxt,
			       NULL, 0, false);
	    }
	    connection->retransmit_pending = true;
	    connection->retransmit_ms = now + TCP_RETRANSMIT_MS;
	}

	if (now - connection->last_activity_ms > TCP_IDLE_MS)
	    conn_close(tcp, connection);
    }
}

int wg_tcp_timeout(WgTcp *tcp)
{
    TcpConn *connection;
    uint64_t now = now_ms();
    int best = -1;

    for (connection = tcp->connections; connection != NULL;
	 connection = connection->next) {
	if (!connection->retransmit_pending)
	    continue;
	if (connection->retransmit_ms <= now)
	    return 0;
	{
	    int remaining =
		(int) (connection->retransmit_ms - now);
	    if (best < 0 || remaining < best)
		best = remaining;
	}
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* Poll integration                                                    */
/* ------------------------------------------------------------------ */

int wg_tcp_pollfds(WgTcp *tcp, struct pollfd *fds, int max)
{
    TcpConn *connection;
    int count = 0;

    for (connection = tcp->connections; connection != NULL;
	 connection = connection->next) {
	if (connection->backend_fd < 0 || count >= max)
	    continue;
	tcp->poll_connections[count] = connection;
	fds[count].fd = connection->backend_fd;
	fds[count].events = POLLIN | (connection->in_len > 0 ? POLLOUT : 0);
	fds[count].revents = 0;
	count++;
    }
    return count;
}

void wg_tcp_poll(WgTcp *tcp, struct pollfd *fds, int count)
{
    int index;

    for (index = 0; index < count; index++) {
	TcpConn *connection = tcp->poll_connections[index];

	if (connection == NULL)
	    continue;
	if (fds[index].revents & POLLOUT)
	    backend_flush(connection);
	if (fds[index].revents & (POLLIN | POLLHUP | POLLERR))
	    backend_read(tcp, connection);
    }
}

/* ------------------------------------------------------------------ */
/* Public interface                                                    */
/* ------------------------------------------------------------------ */

WgTcp *wg_tcp_new(WgTcpSend send, void *opaque)
{
    WgTcp *tcp = calloc(1, sizeof(*tcp));

    if (tcp == NULL)
	return NULL;
    tcp->send = send;
    tcp->opaque = opaque;
    return tcp;
}

void wg_tcp_free(WgTcp *tcp)
{
    if (tcp == NULL)
	return;
    while (tcp->connections != NULL)
	conn_close(tcp, tcp->connections);
    free(tcp);
}

int wg_tcp_add_forward(WgTcp *tcp, uint32_t local_ip, uint16_t local_port,
		       uint32_t host_ip, uint16_t host_port)
{
    WgForward *forward;

    if (tcp->forward_count >= TCP_MAX_FORWARDS)
	return -1;
    forward = &tcp->forwards[tcp->forward_count++];
    forward->local_ip = local_ip;
    forward->local_port = local_port;
    forward->host_ip = host_ip;
    forward->host_port = host_port;
    return 0;
}

int wg_tcp_connection_count(WgTcp *tcp)
{
    return tcp->connection_count;
}
