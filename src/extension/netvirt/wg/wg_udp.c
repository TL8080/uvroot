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

#include "extension/netvirt/wg/wg_udp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define UDP_IDLE_MS	30000
#define UDP_MAX_ENTRIES	64
#define UDP_MAX_FORWARDS 8

typedef struct UdpForward {
    uint32_t local_ip;		/* network order */
    uint16_t local_port;	/* host order */
    uint32_t host_ip;
    uint16_t host_port;
} UdpForward;

typedef struct UdpEntry {
    struct UdpEntry *next;
    uint32_t remote_ip;		/* network order */
    uint16_t remote_port;	/* host order */
    uint32_t local_ip;
    uint16_t local_port;
    int fd;			/* connected to the local service */
    uint64_t last_used_ms;
} UdpEntry;

struct WgUdp {
    WgUdpSend send;
    void *opaque;
    UdpForward forwards[UDP_MAX_FORWARDS];
    int forward_count;
    UdpEntry *entries;
    int entry_count;
    UdpEntry *poll_entries[UDP_MAX_ENTRIES];
};

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

static const UdpForward *forward_for(WgUdp *forward, uint32_t local_ip,
				     uint16_t local_port)
{
    int index;

    for (index = 0; index < forward->forward_count; index++) {
	if (forward->forwards[index].local_ip == local_ip
	    && forward->forwards[index].local_port == local_port)
	    return &forward->forwards[index];
    }
    return NULL;
}

static UdpEntry *entry_find(WgUdp *forward, uint32_t remote_ip,
			    uint16_t remote_port, uint32_t local_ip,
			    uint16_t local_port)
{
    UdpEntry *entry;

    for (entry = forward->entries; entry != NULL; entry = entry->next) {
	if (entry->remote_ip == remote_ip
	    && entry->remote_port == remote_port
	    && entry->local_ip == local_ip
	    && entry->local_port == local_port)
	    return entry;
    }
    return NULL;
}

static void entry_free(WgUdp *forward, UdpEntry *entry)
{
    UdpEntry **link;

    for (link = &forward->entries; *link != NULL; link = &(*link)->next) {
	if (*link == entry) {
	    *link = entry->next;
	    break;
	}
    }
    close(entry->fd);
    free(entry);
    forward->entry_count--;
}

static UdpEntry *entry_create(WgUdp *forward, const UdpForward *config,
			      uint32_t remote_ip, uint16_t remote_port)
{
    struct sockaddr_in address;
    UdpEntry *entry;
    int fd;

    if (forward->entry_count >= UDP_MAX_ENTRIES)
	return NULL;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
	return NULL;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(config->host_ip);
    address.sin_port = htons(config->host_port);
    if (connect(fd, (struct sockaddr *) &address, sizeof(address)) < 0) {
	close(fd);
	return NULL;
    }
    (void) fcntl(fd, F_SETFL, O_NONBLOCK);
    (void) fcntl(fd, F_SETFD, FD_CLOEXEC);

    entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
	close(fd);
	return NULL;
    }
    entry->remote_ip = remote_ip;
    entry->remote_port = remote_port;
    entry->local_ip = config->local_ip;
    entry->local_port = config->local_port;
    entry->fd = fd;
    entry->last_used_ms = now_ms();
    entry->next = forward->entries;
    forward->entries = entry;
    forward->entry_count++;
    return entry;
}

static void entry_reply(WgUdp *forward, UdpEntry *entry, const uint8_t *payload,
			size_t length)
{
    uint8_t packet[2048];
    uint32_t sum;

    if (20 + 8 + length > sizeof(packet))
	return;
    memset(packet, 0, 20 + 8);
    packet[0] = 0x45;
    wr16(packet + 2, (uint16_t) (20 + 8 + length));
    packet[8] = 64;
    packet[9] = 17;		/* UDP */
    wr32(packet + 12, entry->local_ip);
    wr32(packet + 16, entry->remote_ip);
    wr16(packet + 10, checksum_finish(checksum_add(0, packet, 20)));

    wr16(packet + 20, entry->local_port);
    wr16(packet + 22, entry->remote_port);
    wr16(packet + 24, (uint16_t) (8 + length));
    if (length > 0)
	memcpy(packet + 28, payload, length);

    sum = checksum_add(0, packet + 12, 8);
    sum += 17;
    sum += (uint16_t) (8 + length);
    sum = checksum_add(sum, packet + 20, 8 + length);
    {
	uint16_t checksum = checksum_finish(sum);

	wr16(packet + 26, checksum == 0 ? 0xFFFF : checksum);
    }
    (void) forward->send(forward->opaque, packet, 20 + 8 + length);
}

int wg_udp_input(WgUdp *forward, const uint8_t *packet, size_t length)
{
    size_t ip_header_length;
    const uint8_t *header;
    uint32_t source, destination;
    uint16_t source_port, destination_port, udp_length;
    const UdpForward *config;
    UdpEntry *entry;

    if (length < 20 || (packet[0] >> 4) != 4 || packet[9] != 17)
	return 0;
    ip_header_length = (size_t) (packet[0] & 0x0F) * 4;
    if (ip_header_length < 20 || length < ip_header_length + 8)
	return 0;
    header = packet + ip_header_length;
    source = rd32(packet + 12);
    destination = rd32(packet + 16);
    source_port = rd16(header + 0);
    destination_port = rd16(header + 2);
    udp_length = rd16(header + 4);
    if (udp_length < 8 || ip_header_length + udp_length > length)
	udp_length = (uint16_t) (length - ip_header_length);

    config = forward_for(forward, destination, destination_port);
    if (config == NULL)
	return 0;

    entry = entry_find(forward, source, source_port, destination,
		       destination_port);
    if (entry == NULL)
	entry = entry_create(forward, config, source, source_port);
    if (entry == NULL)
	return 1;

    (void) send(entry->fd, header + 8, udp_length - 8, 0);
    entry->last_used_ms = now_ms();
    return 1;
}

void wg_udp_poll(WgUdp *forward, struct pollfd *fds, int count)
{
    uint8_t buffer[2048];
    int index;

    for (index = 0; index < count; index++) {
	UdpEntry *entry = forward->poll_entries[index];
	ssize_t length;

	if (entry == NULL)
	    continue;
	if (!(fds[index].revents & (POLLIN | POLLERR | POLLHUP)))
	    continue;
	length = recv(entry->fd, buffer, sizeof(buffer), 0);
	if (length <= 0)
	    continue;
	entry->last_used_ms = now_ms();
	entry_reply(forward, entry, buffer, (size_t) length);
    }
}

int wg_udp_pollfds(WgUdp *forward, struct pollfd *fds, int max)
{
    UdpEntry *entry;
    int count = 0;

    for (entry = forward->entries; entry != NULL; entry = entry->next) {
	if (count >= max)
	    break;
	forward->poll_entries[count] = entry;
	fds[count].fd = entry->fd;
	fds[count].events = POLLIN;
	fds[count].revents = 0;
	count++;
    }
    return count;
}

void wg_udp_tick(WgUdp *forward)
{
    UdpEntry *entry;
    UdpEntry *next;
    uint64_t now = now_ms();

    for (entry = forward->entries; entry != NULL; entry = next) {
	next = entry->next;
	if (now - entry->last_used_ms > UDP_IDLE_MS)
	    entry_free(forward, entry);
    }
}

WgUdp *wg_udp_new(WgUdpSend send, void *opaque)
{
    WgUdp *forward = calloc(1, sizeof(*forward));

    if (forward == NULL)
	return NULL;
    forward->send = send;
    forward->opaque = opaque;
    return forward;
}

void wg_udp_free(WgUdp *forward)
{
    if (forward == NULL)
	return;
    while (forward->entries != NULL)
	entry_free(forward, forward->entries);
    free(forward);
}

int wg_udp_add_forward(WgUdp *forward, uint32_t local_ip,
		       uint16_t local_port, uint32_t host_ip,
		       uint16_t host_port)
{
    UdpForward *config;

    if (forward->forward_count >= UDP_MAX_FORWARDS)
	return -1;
    config = &forward->forwards[forward->forward_count++];
    config->local_ip = local_ip;
    config->local_port = local_port;
    config->host_ip = host_ip;
    config->host_port = host_port;
    return 0;
}
