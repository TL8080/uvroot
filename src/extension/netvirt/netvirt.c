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

#include <arpa/inet.h>		/* inet_pton(3), */
#include <errno.h>		/* E*, */
#include <net/if.h>		/* IFF_*, IFNAMSIZ, */
#include <netinet/in.h>		/* AF_INET, */
#include <stdbool.h>		/* bool, */
#include <stdint.h>		/* uint*_t, */
#include <stdio.h>		/* snprintf(3), */
#include <stdlib.h>		/* strtol(3), */
#include <string.h>		/* str*(), memcpy(3), */
#include <sys/socket.h>		/* AF_NETLINK, socket(2), */
#include <sys/uio.h>		/* struct iovec, */
#include <talloc.h>		/* talloc_*, */
#include <unistd.h>		/* close(2), */

#include "attribute.h"		/* UNUSED, */
#include "cli/note.h"		/* note(), VERBOSE(), */
#include "extension/extension.h"
#include "extension/netvirt/netvirt.h"
#include "syscall/syscall.h"
#include "syscall/sysnum.h"
#include "tracee/abi.h"		/* get_abi(), sizeof_word(), */
#include "tracee/mem.h"		/* read_data(3), peek_reg(), ... */
#include "tracee/reg.h"
#include "tracee/tracee.h"

#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
#ifndef NETLINK_ROUTE
#define NETLINK_ROUTE 0
#endif

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static NetVirtConfig *config_of(Extension *extension)
{
    return talloc_get_type_abort(extension->config, NetVirtConfig);
}

/*
 * The netvirt syscalls are intercepted at both stages: the enter stage
 * fills the model and the exit stage writes the fabricated result (the
 * kernel executes a harmless syscall in between, see set_sysnum(PR_void)).
 */
static const FilteredSysnum netvirt_sysnums[] = {
    { PR_socket, FILTER_SYSEXIT },
    { PR_open, FILTER_SYSEXIT },
    { PR_openat, FILTER_SYSEXIT },
    { PR_close, FILTER_SYSEXIT },
    { PR_dup, FILTER_SYSEXIT },
    { PR_dup2, FILTER_SYSEXIT },
    { PR_dup3, FILTER_SYSEXIT },
    { PR_fcntl, FILTER_SYSEXIT },
    { PR_fcntl64, FILTER_SYSEXIT },
    { PR_ioctl, FILTER_SYSEXIT },
    { PR_sendmsg, FILTER_SYSEXIT },
    { PR_recvmsg, FILTER_SYSEXIT },
    { PR_sendto, FILTER_SYSEXIT },
    { PR_recvfrom, FILTER_SYSEXIT },
    FILTERED_SYSNUM_END,
};

static bool is_enabled(NetVirtConfig *config)
{
    return config != NULL && config->enabled;
}

/* ------------------------------------------------------------------ */
/* Device / address / route model                                      */
/* ------------------------------------------------------------------ */

NetVirtDev *netvirt_dev_by_name(NetVirtConfig *config, const char *name)
{
    int i;

    if (config == NULL || name == NULL)
	return NULL;

    for (i = 0; i < config->ndevs; i++) {
	if (strcmp(config->devs[i].name, name) == 0)
	    return &config->devs[i];
    }
    return NULL;
}

NetVirtDev *netvirt_dev_by_index(NetVirtConfig *config, int index)
{
    int i;

    if (config == NULL)
	return NULL;

    for (i = 0; i < config->ndevs; i++) {
	if (config->devs[i].index == index)
	    return &config->devs[i];
    }
    return NULL;
}

NetVirtDev *netvirt_dev_alloc(NetVirtConfig *config, const char *name,
			      const char *kind)
{
    NetVirtDev *dev;

    if (config->ndevs >= NETVIRT_MAX_DEVS)
	return NULL;

    dev = &config->devs[config->ndevs++];
    memset(dev, 0, sizeof(*dev));
    snprintf(dev->name, sizeof(dev->name), "%s", name);
    snprintf(dev->kind, sizeof(dev->kind), "%s",
	     kind != NULL ? kind : "dummy");
    dev->index = config->next_index++;
    dev->mtu = NETVIRT_MTU_DEFAULT;
    dev->txqlen = 1000;
    dev->operstate = NETVIRT_OPER_DOWN;
    dev->arphrd = NETVIRT_ARPHRD_ETHER;
    dev->carrier = false;
    dev->bridge = config->default_bridge;

    /* The kernel assigns indexes from the bottom of the range; the
     * loopback is always 1 in a real namespace, keep it that way.  */
    if (strcmp(dev->name, "lo") == 0 && config->next_index <= 2)
	dev->index = 1;
    if (strcmp(dev->name, NETVIRT_VETH_NAME) == 0)
	dev->index = 2;
    if (strcmp(dev->name, NETVIRT_VTUN_NAME) == 0)
	dev->index = 3;

    return dev;
}

void netvirt_dev_remove(NetVirtConfig *config, NetVirtDev *dev)
{
    int i;

    if (config == NULL || dev == NULL)
	return;

    /* Drop the routes that pointed at it.  */
    for (i = 0; i < config->nroutes; i++) {
	if (config->routes[i].oif == dev->index
	    || strcmp(config->routes[i].oif_name, dev->name) == 0) {
	    memmove(&config->routes[i], &config->routes[i + 1],
		    (config->nroutes - i - 1) * sizeof(config->routes[0]));
	    config->nroutes--;
	    i--;
	}
    }

    i = (int)(dev - config->devs);
    if (i < 0 || i >= config->ndevs)
	return;
    memmove(dev, dev + 1, (config->ndevs - i - 1) * sizeof(config->devs[0]));
    config->ndevs--;
    memset(&config->devs[config->ndevs], 0, sizeof(config->devs[0]));
}

bool netvirt_addr_equal(const NetVirtAddr *a, int family,
			const uint8_t addr[16], uint8_t prefix)
{
    size_t len = (family == AF_INET) ? 4 : 16;

    return a->family == family && a->prefix == prefix
	&& memcmp(a->addr, addr, len) == 0;
}

static void netvirt_addr_broadcast(NetVirtAddr *a)
{
    uint32_t host;
    uint32_t mask;

    if (a->family != AF_INET || a->prefix > 30)
	return;

    memcpy(&host, a->addr, 4);
    host = ntohl(host);
    mask = (a->prefix == 0) ? 0 : (0xFFFFFFFFu << (32 - a->prefix));
    host = (host & mask) | ~mask;
    host = htonl(host);
    memcpy(a->broadcast, &host, 4);
    a->has_broadcast = true;
}

int netvirt_route_add(NetVirtConfig *config, const NetVirtRoute *route)
{
    int i;

    if (config == NULL || config->nroutes >= NETVIRT_MAX_ROUTES)
	return -ENOSPC;

    for (i = 0; i < config->nroutes; i++) {
	NetVirtRoute *r = &config->routes[i];
	size_t len = (route->family == AF_INET) ? 4 : 16;

	if (r->family == route->family
	    && r->prefix == route->prefix
	    && memcmp(r->dst, route->dst, len) == 0
	    && r->oif == route->oif
	    && r->table == route->table
	    && r->has_gw == route->has_gw
	    && (!route->has_gw || memcmp(r->gw, route->gw, len) == 0)) {
	    /* Refresh the mutable fields rather than duplicating.  */
	    r->scope = route->scope;
	    r->protocol = route->protocol;
	    r->type = route->type;
	    r->metric = route->metric;
	    r->connected = route->connected;
	    return 0;
	}
    }

    config->routes[config->nroutes++] = *route;
    return 0;
}

int netvirt_route_del(NetVirtConfig *config, const NetVirtRoute *route)
{
    int i;

    if (config == NULL)
	return -ENOENT;

    for (i = 0; i < config->nroutes; i++) {
	NetVirtRoute *r = &config->routes[i];
	size_t len = (route->family == AF_INET) ? 4 : 16;

	if (r->family != route->family)
	    continue;
	if (route->table != 0 && r->table != route->table)
	    continue;
	if (route->oif != 0 && r->oif != route->oif)
	    continue;
	if (route->has_gw
	    && (!r->has_gw || memcmp(r->gw, route->gw, len) != 0))
	    continue;
	/* A prefix of 255 means "any destination" (ip route flush).  */
	if (route->prefix != 255
	    && (r->prefix != route->prefix
		|| memcmp(r->dst, route->dst, len) != 0))
	    continue;

	memmove(&config->routes[i], &config->routes[i + 1],
		(config->nroutes - i - 1) * sizeof(config->routes[0]));
	config->nroutes--;
	return 0;
    }
    return -ENOENT;
}

void netvirt_route_reindex(NetVirtConfig *config)
{
    int i;

    for (i = 0; i < config->nroutes; i++) {
	NetVirtDev *dev = netvirt_dev_by_name(config,
					      config->routes[i].oif_name);
	config->routes[i].oif = (dev != NULL) ? dev->index : 0;
    }
}

int netvirt_addr_route(NetVirtConfig *config, const NetVirtDev *dev,
		       const NetVirtAddr *addr, bool add)
{
    NetVirtRoute route;
    size_t len = (addr->family == AF_INET) ? 4 : 16;
    uint8_t network[16];
    int i;

    /* A host or point-to-point address carries no connected subnet.  */
    if (addr->prefix >= (addr->family == AF_INET ? 31 : 127))
	return 0;

    memset(network, 0, sizeof(network));
    for (i = 0; i < (int)len; i++) {
	int bits = addr->prefix - i * 8;
	uint8_t m = (bits >= 8) ? 0xFF
	    : (bits <= 0 ? 0 : (uint8_t) (0xFF << (8 - bits)));

	network[i] = addr->addr[i] & m;
    }

    memset(&route, 0, sizeof(route));
    route.family = addr->family;
    memcpy(route.dst, network, len);
    route.prefix = addr->prefix;
    route.oif = dev->index;
    snprintf(route.oif_name, sizeof(route.oif_name), "%s", dev->name);
    route.table = NETVIRT_TABLE_MAIN;
    route.protocol = NETVIRT_PROTO_KERNEL;
    route.scope = NETVIRT_SCOPE_LINK;
    route.type = NETVIRT_RTN_UNICAST;
    route.connected = true;
    route.flags = 0;
    return add ? netvirt_route_add(config, &route)
	: netvirt_route_del(config, &route);
}

int netvirt_addr_add(NetVirtDev *dev, const NetVirtAddr *addr)
{
    int i;

    if (dev == NULL)
	return -EINVAL;

    for (i = 0; i < dev->naddrs; i++) {
	if (netvirt_addr_equal(&dev->addrs[i], addr->family, addr->addr,
			       addr->prefix))
	    return -EEXIST;
    }

    if (dev->naddrs >= NETVIRT_MAX_ADDRS)
	return -ENOSPC;

    dev->addrs[dev->naddrs] = *addr;
    if (dev->addrs[dev->naddrs].label[0] == '\0') {
	size_t length = strlen(dev->name);

	if (length >= sizeof(dev->addrs[dev->naddrs].label))
	    length = sizeof(dev->addrs[dev->naddrs].label) - 1;
	memcpy(dev->addrs[dev->naddrs].label, dev->name, length);
	dev->addrs[dev->naddrs].label[length] = '\0';
    }
    netvirt_addr_broadcast(&dev->addrs[dev->naddrs]);
    dev->naddrs++;
    return 0;
}

int netvirt_addr_del(NetVirtDev *dev, int family, const uint8_t addr[16],
		     uint8_t prefix)
{
    int i;

    if (dev == NULL)
	return -EINVAL;

    for (i = 0; i < dev->naddrs; i++) {
	if (netvirt_addr_equal(&dev->addrs[i], family, addr, prefix)) {
	    memmove(&dev->addrs[i], &dev->addrs[i + 1],
		    (dev->naddrs - i - 1) * sizeof(dev->addrs[0]));
	    dev->naddrs--;
	    return 0;
	}
    }
    return -EADDRNOTAVAIL;
}

/* ------------------------------------------------------------------ */
/* Per-tracee file descriptor tracking                                 */
/* ------------------------------------------------------------------ */

static NetVirtTracee *tracee_state(NetVirtConfig *config, pid_t pid,
				   bool create)
{
    NetVirtTracee *state;

    for (state = config->tracees; state != NULL; state = state->next) {
	if (state->pid == pid)
	    return state;
    }

    if (!create)
	return NULL;

    state = talloc_zero(config, NetVirtTracee);
    if (state == NULL)
	return NULL;
    state->pid = pid;
    state->next = config->tracees;
    config->tracees = state;
    return state;
}

static NetVirtFd *fd_lookup(NetVirtTracee *state, int fd)
{
    NetVirtFd *entry;

    if (state == NULL)
	return NULL;

    for (entry = state->fds; entry != NULL; entry = entry->next) {
	if (entry->fd == fd)
	    return entry;
    }
    return NULL;
}

static NetVirtFd *fd_add(NetVirtTracee *state, int fd)
{
    NetVirtFd *entry;

    if (state == NULL)
	return NULL;

    entry = fd_lookup(state, fd);
    if (entry != NULL)
	return entry;

    entry = talloc_zero(state, NetVirtFd);
    if (entry == NULL)
	return NULL;
    entry->fd = fd;
    entry->next = state->fds;
    state->fds = entry;
    return entry;
}

static void fd_remove(NetVirtTracee *state, int fd)
{
    NetVirtFd **link;

    if (state == NULL)
	return;

    for (link = &state->fds; *link != NULL; link = &(*link)->next) {
	if ((*link)->fd == fd) {
	    NetVirtFd *entry = *link;
	    *link = entry->next;
	    TALLOC_FREE(entry);
	    return;
	}
    }
}

static void fd_dup(NetVirtTracee *state, int oldfd, int newfd)
{
    NetVirtFd *old;
    NetVirtFd *entry;

    if (state == NULL || newfd < 0)
	return;

    old = fd_lookup(state, oldfd);
    if (old == NULL)
	return;

    fd_remove(state, newfd);
    entry = fd_add(state, newfd);
    if (entry == NULL)
	return;
    entry->rtnl = old->rtnl;
    entry->tun = old->tun;
    if (old->tun)
	snprintf(entry->tun_name, sizeof(entry->tun_name), "%s",
		 old->tun_name);
}

static void fd_append(NetVirtFd *entry, const unsigned char *data, size_t len)
{
    NetVirtFrame *frame;

    if (entry == NULL || len == 0)
	return;

    frame = talloc_zero(entry, NetVirtFrame);
    if (frame == NULL)
	return;
    frame->data = talloc_memdup(frame, data, len);
    if (frame->data == NULL) {
	TALLOC_FREE(frame);
	return;
    }
    frame->length = len;
    frame->offset = 0;

    if (entry->tail != NULL)
	entry->tail->next = frame;
    else
	entry->frames = frame;
    entry->tail = frame;
}

/* Exposed to netlink.c so that replies can be queued on a socket.  */
void netvirt_fd_append(NetVirtFd *fd, const void *data, size_t length)
{
    fd_append(fd, data, length);
}

/* ------------------------------------------------------------------ */
/* iovec / msghdr helpers (native ABI only)                            */
/* ------------------------------------------------------------------ */

#define NETVIRT_IOV_MAX 1024
#define NETVIRT_MSG_MAX 65536

static bool native_abi(const Tracee *tracee)
{
    return get_abi(tracee) == ABI_DEFAULT;
}

static ssize_t gather_iov(Tracee *tracee, const struct msghdr *msg,
			  unsigned char *buffer, size_t capacity)
{
    size_t total = 0;
    size_t i;

    if (msg->msg_iovlen > NETVIRT_IOV_MAX)
	return -EINVAL;

    for (i = 0; i < msg->msg_iovlen; i++) {
	struct iovec iov;
	size_t chunk;

	if (read_data(tracee, &iov, (word_t) &msg->msg_iov[i],
		      sizeof(iov)) < 0)
	    return -EFAULT;
	chunk = iov.iov_len;
	if (chunk > capacity - total)
	    chunk = capacity - total;
	if (chunk > 0 && read_data(tracee, buffer + total,
				   (word_t) iov.iov_base, chunk) < 0)
	    return -EFAULT;
	total += chunk;
	if (total >= capacity)
	    break;
    }
    return (ssize_t) total;
}

static size_t iov_total(const Tracee *tracee, const struct msghdr *msg)
{
    size_t total = 0;
    size_t i;

    if (msg->msg_iovlen > NETVIRT_IOV_MAX)
	return 0;

    for (i = 0; i < msg->msg_iovlen; i++) {
	struct iovec iov;

	if (read_data(tracee, &iov, (word_t) &msg->msg_iov[i],
		      sizeof(iov)) < 0)
	    return 0;
	total += iov.iov_len;
    }
    return total;
}

static ssize_t scatter_iov(Tracee *tracee, const struct msghdr *msg,
			   const unsigned char *buffer, size_t length)
{
    size_t done = 0;
    size_t i;

    if (msg->msg_iovlen > NETVIRT_IOV_MAX)
	return -EINVAL;

    for (i = 0; i < msg->msg_iovlen && done < length; i++) {
	struct iovec iov;
	size_t chunk;

	if (read_data(tracee, &iov, (word_t) &msg->msg_iov[i],
		      sizeof(iov)) < 0)
	    return -EFAULT;
	chunk = iov.iov_len;
	if (chunk > length - done)
	    chunk = length - done;
	if (chunk > 0 && write_data(tracee, (word_t) iov.iov_base,
				    buffer + done, chunk) < 0)
	    return -EFAULT;
	done += chunk;
    }
    return (ssize_t) done;
}

struct netvirt_sockaddr_nl {
    uint16_t nl_family;
    uint16_t nl_pad;
    uint32_t nl_pid;
    uint32_t nl_groups;
};

static void fill_sender(Tracee *tracee, struct msghdr *msg)
{
    struct netvirt_sockaddr_nl addr;

    if (msg->msg_name == NULL || msg->msg_namelen < sizeof(addr))
	return;

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = 0;		/* the kernel */
    (void) write_data(tracee, (word_t) msg->msg_name, &addr, sizeof(addr));
    msg->msg_namelen = sizeof(addr);
}

/* ------------------------------------------------------------------ */
/* Syscall interception                                                */
/* ------------------------------------------------------------------ */

static int enter_sendmsg(Tracee *tracee, NetVirtConfig *config, bool is_sendto)
{
    NetVirtTracee *state = tracee_state(config, tracee->pid, true);
    NetVirtFd *entry;
    unsigned char *buffer;
    ssize_t length;
    int fd;

    fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
    entry = fd_lookup(state, fd);
    if (entry == NULL || !entry->rtnl)
	return 0;

    buffer = talloc_size(tracee->ctx, NETVIRT_MSG_MAX);
    if (buffer == NULL)
	return -ENOMEM;

    if (is_sendto) {
	size_t len = (size_t) peek_reg(tracee, CURRENT, SYSARG_3);
	word_t address = peek_reg(tracee, CURRENT, SYSARG_2);

	if (len > NETVIRT_MSG_MAX)
	    len = NETVIRT_MSG_MAX;
	if (read_data(tracee, buffer, address, len) < 0) {
	    talloc_free(buffer);
	    return -EFAULT;
	}
	length = (ssize_t) len;
    } else {
	struct msghdr msg;

	if (!native_abi(tracee)) {
	    talloc_free(buffer);
	    return 0;
	}
	if (read_data(tracee, &msg, peek_reg(tracee, CURRENT, SYSARG_2),
		      sizeof(msg)) < 0) {
	    talloc_free(buffer);
	    return -EFAULT;
	}
	length = gather_iov(tracee, &msg, buffer, NETVIRT_MSG_MAX);
    }

    if (length <= 0) {
	talloc_free(buffer);
	return (length < 0) ? (int) length : 0;
    }

    (void) netvirt_netlink_request(tracee, config, entry, buffer,
				   (size_t) length);
    talloc_free(buffer);

    state->pend_valid = true;
    state->pend_result = length;
    set_sysnum(tracee, PR_void);
    return 1;
}

static int enter_recvmsg(Tracee *tracee, NetVirtConfig *config,
			 bool is_recvfrom)
{
    NetVirtTracee *state = tracee_state(config, tracee->pid, true);
    NetVirtFd *entry;
    int flags;
    long result;
    int fd;

    fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
    entry = fd_lookup(state, fd);
    if (entry == NULL || !entry->rtnl)
	return 0;

    flags = is_recvfrom ? (int) peek_reg(tracee, CURRENT, SYSARG_4)
	: (int) peek_reg(tracee, CURRENT, SYSARG_3);

    if (entry->frames == NULL) {
	result = -EAGAIN;
    } else {
	NetVirtFrame *frame = entry->frames;
	size_t available = frame->length - frame->offset;
	size_t capacity;
	ssize_t chunk;
	bool truncate = (flags & MSG_TRUNC) != 0;
	bool peek = (flags & MSG_PEEK) != 0;

	if (is_recvfrom) {
	    capacity = (size_t) peek_reg(tracee, CURRENT, SYSARG_3);
	    chunk = (available < capacity) ? (ssize_t) available
		: (ssize_t) capacity;
	    if (chunk > 0
		&& write_data(tracee, peek_reg(tracee, CURRENT, SYSARG_2),
			      frame->data + frame->offset,
			      (size_t) chunk) < 0)
		return -EFAULT;
	} else {
	    struct msghdr msg;
	    word_t address;

	    if (!native_abi(tracee))
		return 0;

	    address = peek_reg(tracee, CURRENT, SYSARG_2);
	    if (read_data(tracee, &msg, address, sizeof(msg)) < 0)
		return -EFAULT;

	    capacity = iov_total(tracee, &msg);
	    chunk = scatter_iov(tracee, &msg, frame->data + frame->offset,
				(available < capacity) ? available : capacity);
	    if (chunk < 0)
		return (int) chunk;

	    fill_sender(tracee, &msg);
	    /* MSG_TRUNC is reported back when the datagram did not fit,
	     * exactly like the kernel does.  */
	    msg.msg_flags = (available > capacity) ? MSG_TRUNC : 0;
	    (void) write_data(tracee, address, &msg, sizeof(msg));
	}

	/* MSG_PEEK does not consume the datagram; MSG_TRUNC makes the
	 * return value the real datagram size, whatever was copied.
	 * ip(8) uses MSG_PEEK|MSG_TRUNC with an empty iovec to size its
	 * buffer before the real read.  */
	if (!peek) {
	    frame->offset += (size_t) chunk;
	    if (frame->offset >= frame->length) {
		entry->frames = frame->next;
		if (entry->frames == NULL)
		    entry->tail = NULL;
		frame->next = NULL;
		TALLOC_FREE(frame);
	    }
	}

	result = truncate ? (long) available : (long) chunk;
    }

    state->pend_valid = true;
    state->pend_result = result;
    set_sysnum(tracee, PR_void);
    return 1;
}

/*
 * The container opening /dev/net/tun must not touch the host device (it
 * would need CAP_NET_ADMIN and would escape the virtual network).  The
 * open is turned into a dup() of the virtual TUN end the WireGuard
 * engine is attached to.
 */
static int enter_open(Tracee *tracee, NetVirtConfig *config)
{
    NetVirtTracee *state;
    char path[PATH_MAX];
    Sysnum sysnum = get_sysnum(tracee, ORIGINAL);
    Reg reg;

    if (!config->wg_engine_started || config->tun_guest_fd < 0)
	return 0;

    reg = (sysnum == PR_open) ? SYSARG_1 : SYSARG_2;
    if (get_sysarg_path(tracee, path, reg) < 0)
	return 0;
    if (strcmp(path, "/dev/net/tun") != 0)
	return 0;

    state = tracee_state(config, tracee->pid, true);
    if (state == NULL)
	return -ENOMEM;

    set_sysnum(tracee, PR_dup);
    poke_reg(tracee, SYSARG_1, (word_t) config->tun_guest_fd);
    state->tun_open_pending = true;
    VERBOSE(tracee, 4, "netvirt: virtual /dev/net/tun opened (fd %d)",
	    config->tun_guest_fd);
    return 1;
}

static int enter_ioctl(Tracee *tracee, NetVirtConfig *config)
{
    NetVirtTracee *state = tracee_state(config, tracee->pid, true);
    unsigned long request;
    word_t argument;
    NetVirtFd *entry;
    long result = 0;
    int handled;

    request = (unsigned long) peek_reg(tracee, CURRENT, SYSARG_2);
    argument = peek_reg(tracee, CURRENT, SYSARG_3);
    entry = fd_lookup(state, (int) peek_reg(tracee, CURRENT, SYSARG_1));

    if (entry != NULL && entry->tun)
	handled = netvirt_tun_ioctl(tracee, config, entry, request, argument,
				    &result);
    else
	handled = netvirt_ioctl_request(tracee, config, request, argument,
					&result);
    if (handled <= 0)
	return handled;

    state->pend_valid = true;
    state->pend_result = result;
    set_sysnum(tracee, PR_void);
    return 1;
}

static int enter_syscall(Tracee *tracee, NetVirtConfig *config)
{
    switch (get_sysnum(tracee, ORIGINAL)) {
    case PR_sendmsg:
	return enter_sendmsg(tracee, config, false);

    case PR_sendto:
	return enter_sendmsg(tracee, config, true);

    case PR_recvmsg:
	return enter_recvmsg(tracee, config, false);

    case PR_recvfrom:
	return enter_recvmsg(tracee, config, true);

    case PR_ioctl:
	return enter_ioctl(tracee, config);

    case PR_open:
    case PR_openat:
	return enter_open(tracee, config);

    default:
	return 0;
    }
}

static int exit_syscall(Tracee *tracee, NetVirtConfig *config)
{
    NetVirtTracee *state = tracee_state(config, tracee->pid, true);
    Sysnum sysnum = get_sysnum(tracee, ORIGINAL);
    long result = (long) peek_reg(tracee, CURRENT, SYSARG_RESULT);

    switch (sysnum) {
    case PR_socket:{
	int domain = (int) peek_reg(tracee, ORIGINAL, SYSARG_1);
	int protocol = (int) peek_reg(tracee, ORIGINAL, SYSARG_3);

	if (result >= 0 && domain == AF_NETLINK && protocol == NETLINK_ROUTE) {
	    NetVirtFd *entry = fd_add(state, (int) result);
	    if (entry != NULL)
		entry->rtnl = true;
	    VERBOSE(tracee, 4, "netvirt: netlink route socket %ld", result);
	}
	break;
    }

    case PR_close:
	if (result == 0)
	    fd_remove(state, (int) peek_reg(tracee, ORIGINAL, SYSARG_1));
	break;

    case PR_dup:
    case PR_dup2:
    case PR_dup3:
	if (result >= 0)
	    fd_dup(state, (int) peek_reg(tracee, ORIGINAL, SYSARG_1),
		   (int) result);
	break;

    case PR_fcntl:
    case PR_fcntl64:{
	int command = (int) peek_reg(tracee, ORIGINAL, SYSARG_2);

	if (result >= 0 && (command == 0 /* F_DUPFD */
			    || command == 1030 /* F_DUPFD_CLOEXEC */))
	    fd_dup(state, (int) peek_reg(tracee, ORIGINAL, SYSARG_1),
		   (int) result);
	break;
    }

    case PR_getsockname:{
	int fd = (int) peek_reg(tracee, ORIGINAL, SYSARG_1);
	NetVirtFd *entry = fd_lookup(state, fd);

	/* Learn the port id the kernel assigned to the netlink socket:
	 * every synthetic reply must carry it (see emit() in netlink.c).  */
	if (result == 0 && entry != NULL && entry->rtnl) {
	    struct netvirt_sockaddr_nl address;
	    word_t pointer = peek_reg(tracee, ORIGINAL, SYSARG_2);

	    if (read_data(tracee, &address, pointer, sizeof(address)) >= 0
		&& address.nl_family == AF_NETLINK)
		entry->local_pid = address.nl_pid;
	}
	break;
    }

    default:
	break;
    }

    if (state->tun_open_pending) {
	state->tun_open_pending = false;
	if (result >= 0) {
	    NetVirtFd *entry = fd_add(state, (int) result);

	    if (entry != NULL)
		entry->tun = true;
	}
    }

    if (state->pend_valid) {
	state->pend_valid = false;
	poke_reg(tracee, SYSARG_RESULT, (word_t) state->pend_result);
	return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Model construction from the command line                            */
/* ------------------------------------------------------------------ */

static void addr_init(NetVirtAddr *addr, int family, const uint8_t data[16],
		      uint8_t prefix, uint8_t scope, const char *label)
{
    memset(addr, 0, sizeof(*addr));
    addr->family = family;
    addr->prefix = prefix;
    addr->scope = scope;
    memcpy(addr->addr, data, (family == AF_INET) ? 4 : 16);
    if (label != NULL)
	snprintf(addr->label, sizeof(addr->label), "%s", label);
}

static int parse_cidr(const char *text, int *family, uint8_t address[16],
		      uint8_t *prefix)
{
    char buffer[128];
    char *slash;
    int value;

    if (text == NULL || strlen(text) >= sizeof(buffer))
	return -EINVAL;
    snprintf(buffer, sizeof(buffer), "%s", text);

    slash = strchr(buffer, '/');
    if (slash != NULL) {
	*slash = '\0';
	slash++;
	value = atoi(slash);
	if (value < 0)
	    value = 0;
    } else
	value = -1;

    if (inet_pton(AF_INET, buffer, address) == 1) {
	*family = AF_INET;
	if (value < 0 || value > 32)
	    value = 32;
    } else if (inet_pton(AF_INET6, buffer, address) == 1) {
	*family = AF_INET6;
	if (value < 0 || value > 128)
	    value = 128;
    } else
	return -EINVAL;

    *prefix = (uint8_t) value;
    return 0;
}

static void device_defaults(NetVirtConfig *config)
{
    NetVirtDev *lo;
    NetVirtDev *veth0;
    NetVirtDev *vtun;
    NetVirtAddr addr;
    uint8_t data[16];
    int family;
    uint8_t prefix;
    NetVirtRoute route;

    if (config->ndevs > 0)
	return;

    /* Loopback: always present, like in a real network namespace.  */
    lo = netvirt_dev_alloc(config, "lo", "loopback");
    if (lo != NULL) {
	lo->flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING;
	lo->mtu = 65536;
	lo->txqlen = 1000;
	lo->operstate = NETVIRT_OPER_UP;
	lo->arphrd = NETVIRT_ARPHRD_LOOPBACK;
	lo->carrier = true;
	lo->bridge = NETVIRT_BRIDGE_NONE;	/* the loopback is local */
	memset(data, 0, sizeof(data));
	data[0] = 127;
	data[3] = 1;
	addr_init(&addr, AF_INET, data, 8, NETVIRT_SCOPE_HOST, "lo");
	(void) netvirt_addr_add(lo, &addr);
	memset(data, 0, sizeof(data));
	data[15] = 1;
	addr_init(&addr, AF_INET6, data, 128, NETVIRT_SCOPE_HOST, "lo");
	(void) netvirt_addr_add(lo, &addr);
    }

    /* veth0: ordinary network I/O, bridged over WireGuard.  */
    veth0 = netvirt_dev_alloc(config, NETVIRT_VETH_NAME, "veth");
    if (veth0 != NULL) {
	static const uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x76, 0x65, 0x30 };

	veth0->flags = IFF_UP | IFF_BROADCAST | IFF_MULTICAST | IFF_RUNNING;
	veth0->mtu = NETVIRT_MTU_DEFAULT;
	veth0->operstate = NETVIRT_OPER_UP;
	veth0->carrier = true;
	memcpy(veth0->mac, mac, sizeof(mac));
	veth0->has_mac = true;
	veth0->bridge = NETVIRT_BRIDGE_WG_USER;
	if (parse_cidr(NETVIRT_VETH_ADDR "/" "24", &family, data,
		       &prefix) == 0) {
	    addr_init(&addr, family, data, prefix, NETVIRT_SCOPE_UNIVERSE,
		      NETVIRT_VETH_NAME);
	    if (netvirt_addr_add(veth0, &addr) == 0)
		(void) netvirt_addr_route(config, veth0, &addr, true);
	}
	memset(&route, 0, sizeof(route));
	route.family = AF_INET;
	route.prefix = 0;
	route.has_gw = true;
	(void) parse_cidr(NETVIRT_VETH_GW, &family, route.gw, &prefix);
	route.oif = veth0->index;
	snprintf(route.oif_name, sizeof(route.oif_name), "%s", veth0->name);
	route.table = NETVIRT_TABLE_MAIN;
	route.protocol = NETVIRT_PROTO_BOOT;
	route.scope = NETVIRT_SCOPE_UNIVERSE;
	route.type = NETVIRT_RTN_UNICAST;
	(void) netvirt_route_add(config, &route);
    }

    /* vtun: a point-to-point device for tailscale/intranet traffic.  */
    vtun = netvirt_dev_alloc(config, NETVIRT_VTUN_NAME, "tun");
    if (vtun != NULL) {
	vtun->flags = IFF_UP | IFF_POINTOPOINT | IFF_NOARP
	    | IFF_MULTICAST | IFF_RUNNING;
	vtun->mtu = 1420;
	vtun->operstate = NETVIRT_OPER_UP;
	vtun->carrier = true;
	vtun->arphrd = NETVIRT_ARPHRD_NONE;
	vtun->bridge = NETVIRT_BRIDGE_WG_USER;
	if (parse_cidr(NETVIRT_VTUN_ADDR, &family, data, &prefix) == 0) {
	    addr_init(&addr, family, data, prefix, NETVIRT_SCOPE_UNIVERSE,
		      NETVIRT_VTUN_NAME);
	    if (netvirt_addr_add(vtun, &addr) == 0)
		(void) netvirt_addr_route(config, vtun, &addr, true);
	}
	memset(&route, 0, sizeof(route));
	if (parse_cidr(NETVIRT_VTUN_ROUTE "/10", &family, route.dst,
		       &prefix) == 0) {
	    route.family = family;
	    route.prefix = prefix;
	    route.oif = vtun->index;
	    snprintf(route.oif_name, sizeof(route.oif_name), "%s",
		     vtun->name);
	    route.table = NETVIRT_TABLE_MAIN;
	    route.protocol = NETVIRT_PROTO_BOOT;
	    route.scope = NETVIRT_SCOPE_LINK;
	    route.type = NETVIRT_RTN_UNICAST;
	    (void) netvirt_route_add(config, &route);
	}
    }
}

static NetVirtDev *device_ensure(NetVirtConfig *config, const char *name)
{
    NetVirtDev *dev = netvirt_dev_by_name(config, name);

    if (dev != NULL)
	return dev;

    dev = netvirt_dev_alloc(config, name, "dummy");
    if (dev == NULL)
	return NULL;
    dev->flags = IFF_UP | IFF_BROADCAST | IFF_MULTICAST | IFF_RUNNING;
    dev->operstate = NETVIRT_OPER_UP;
    dev->carrier = true;
    dev->has_mac = true;
    dev->mac[0] = 0x02;
    dev->mac[5] = (uint8_t) dev->index;
    return dev;
}

static int parse_device_spec(Tracee *tracee, NetVirtConfig *config,
			     const char *spec)
{
    char *copy;
    char *cursor;
    const char *name = NULL;
    NetVirtDev *dev;
    int status = 0;

    copy = talloc_strdup(tracee->ctx, spec);
    if (copy == NULL)
	return -ENOMEM;

    cursor = copy;
    while (cursor != NULL && *cursor != '\0') {
	char *comma = strchr(cursor, ',');
	char *value;
	bool up = false;
	bool down = false;

	if (comma != NULL)
	    *comma = '\0';

	value = strchr(cursor, '=');
	if (value != NULL)
	    *value++ = '\0';

	if (name == NULL && value == NULL) {
	    name = cursor;
	    if (device_ensure(config, name) == NULL) {
		status = -ENOSPC;
		break;
	    }
	    cursor = (comma != NULL) ? comma + 1 : NULL;
	    continue;
	}

	if (name == NULL && value != NULL && strcmp(cursor, "name") == 0) {
	    name = value;
	    if (device_ensure(config, name) == NULL) {
		status = -ENOSPC;
		break;
	    }
	    cursor = (comma != NULL) ? comma + 1 : NULL;
	    continue;
	}

	if (name == NULL) {
	    status = -EINVAL;
	    break;
	}

	dev = device_ensure(config, name);
	if (dev == NULL) {
	    status = -ENOSPC;
	    break;
	}

	if (value == NULL) {
	    if (strcmp(cursor, "up") == 0)
		up = true;
	    else if (strcmp(cursor, "down") == 0)
		down = true;
	    else {
		status = -EINVAL;
		break;
	    }
	    if (up)
		dev->flags |= IFF_UP | IFF_RUNNING, dev->operstate =
		    NETVIRT_OPER_UP, dev->carrier = true;
	    if (down)
		dev->flags &= ~(unsigned)(IFF_UP | IFF_RUNNING),
		    dev->operstate = NETVIRT_OPER_DOWN, dev->carrier = false;
	} else if (strcmp(cursor, "addr") == 0
		   || strcmp(cursor, "address") == 0) {
	    uint8_t data[16];
	    int family;
	    uint8_t prefix = 0;
	    NetVirtAddr addr;

	    if (parse_cidr(value, &family, data, &prefix) < 0) {
		status = -EINVAL;
		break;
	    }
	    addr_init(&addr, family, data, prefix,
		      (family == AF_INET && prefix == 8) ?
		      NETVIRT_SCOPE_HOST : NETVIRT_SCOPE_UNIVERSE, name);
	    if (netvirt_addr_add(dev, &addr) == 0)
		(void) netvirt_addr_route(config, dev, &addr, true);
	} else if (strcmp(cursor, "mtu") == 0) {
	    dev->mtu = atoi(value);
	} else if (strcmp(cursor, "txqlen") == 0) {
	    dev->txqlen = (unsigned) atoi(value);
	} else if (strcmp(cursor, "kind") == 0) {
	    snprintf(dev->kind, sizeof(dev->kind), "%s", value);
	} else if (strcmp(cursor, "mac") == 0) {
	    unsigned int b[6];
	    if (sscanf(value, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2],
		       &b[3], &b[4], &b[5]) == 6) {
		int i;
		for (i = 0; i < 6; i++)
		    dev->mac[i] = (uint8_t) b[i];
		dev->has_mac = true;
	    }
	} else if (strcmp(cursor, "wg") == 0) {
	    dev->bridge = NETVIRT_BRIDGE_WG_USER;
	    if (value[0] != '\0') {
		dev->wg_conf = talloc_strdup(config, value);
		if (dev->wg_conf == NULL)
		    status = -ENOMEM;
	    }
	} else {
	    status = -EINVAL;
	    break;
	}

	cursor = (comma != NULL) ? comma + 1 : NULL;
    }

    talloc_free(copy);

    if (status < 0) {
	note(tracee, ERROR, USER, "netvirt: invalid interface \"%s\"", spec);
	return status;
    }
    return 0;
}

static int parse_route_spec(Tracee *tracee, NetVirtConfig *config,
			    const char *spec)
{
    char *copy;
    char *save = NULL;
    char *token;
    NetVirtRoute route;
    bool have_dst = false;

    memset(&route, 0, sizeof(route));
    route.table = NETVIRT_TABLE_MAIN;
    route.protocol = NETVIRT_PROTO_BOOT;
    route.scope = NETVIRT_SCOPE_UNIVERSE;
    route.type = NETVIRT_RTN_UNICAST;
    route.prefix = (uint8_t) -1;

    copy = talloc_strdup(tracee->ctx, spec);
    if (copy == NULL)
	return -ENOMEM;

    for (token = strtok_r(copy, " \t", &save); token != NULL;
	 token = strtok_r(NULL, " \t", &save)) {
	if (strcmp(token, "via") == 0 || strcmp(token, "gw") == 0) {
	    char *gateway = strtok_r(NULL, " \t", &save);
	    uint8_t prefix;
	    int family;
	    if (gateway == NULL
		|| parse_cidr(gateway, &family, route.gw, &prefix) < 0) {
		talloc_free(copy);
		note(tracee, ERROR, USER, "netvirt: invalid gateway in \"%s\"",
		     spec);
		return -EINVAL;
	    }
	    route.family = family;
	    route.has_gw = true;
	} else if (strcmp(token, "dev") == 0) {
	    char *name = strtok_r(NULL, " \t", &save);
	    NetVirtDev *dev;
	    if (name == NULL) {
		talloc_free(copy);
		return -EINVAL;
	    }
	    dev = netvirt_dev_by_name(config, name);
	    if (dev == NULL) {
		talloc_free(copy);
		note(tracee, ERROR, USER,
		     "netvirt: unknown interface \"%s\" in \"%s\"", name,
		     spec);
		return -ENODEV;
	    }
	    route.oif = dev->index;
	    snprintf(route.oif_name, sizeof(route.oif_name), "%s", name);
	} else if (strcmp(token, "metric") == 0) {
	    char *value = strtok_r(NULL, " \t", &save);
	    route.metric = (uint32_t) atoi(value != NULL ? value : "0");
	} else if (strcmp(token, "table") == 0) {
	    char *value = strtok_r(NULL, " \t", &save);
	    route.table = atoi(value != NULL ? value : "254");
	} else if (!have_dst) {
	    uint8_t prefix;
	    int family;

	    if (strcmp(token, "default") == 0) {
		/* ``ip route`` spells the default route this way.  */
		if (route.family == AF_UNSPEC)
		    route.family = AF_INET;
		route.prefix = 0;
		have_dst = true;
		continue;
	    }
	    if (parse_cidr(token, &family, route.dst, &prefix) < 0) {
		talloc_free(copy);
		note(tracee, ERROR, USER, "netvirt: invalid route \"%s\"",
		     spec);
		return -EINVAL;
	    }
	    route.family = family;
	    route.prefix = prefix;
	    have_dst = true;
	} else {
	    talloc_free(copy);
	    note(tracee, ERROR, USER, "netvirt: invalid route \"%s\"", spec);
	    return -EINVAL;
	}
    }

    if (route.prefix == (uint8_t) -1)
	route.prefix = (route.family == AF_INET6) ? 128 : 0;

    if (route.prefix > 0 && route.scope == NETVIRT_SCOPE_UNIVERSE
	&& !route.has_gw && route.oif != 0)
	route.scope = NETVIRT_SCOPE_LINK;

    talloc_free(copy);
    return netvirt_route_add(config, &route);
}

/* ------------------------------------------------------------------ */
/* Extension plumbing                                                  */
/* ------------------------------------------------------------------ */

static Extension *netvirt_extension(Tracee *tracee)
{
    Extension *extension = get_extension(tracee, netvirt_callback);

    if (extension == NULL) {
	if (initialize_extension(tracee, netvirt_callback, NULL) < 0)
	    return NULL;
	extension = get_extension(tracee, netvirt_callback);
    }
    return extension;
}

static NetVirtConfig *netvirt_get_config(Tracee *tracee, bool create)
{
    Extension *extension;

    if (create)
	extension = netvirt_extension(tracee);
    else
	extension = get_extension(tracee, netvirt_callback);

    return (extension != NULL) ? config_of(extension) : NULL;
}

int netvirt_enable(Tracee *tracee)
{
    NetVirtConfig *config = netvirt_get_config(tracee, true);

    if (config == NULL)
	return -ENOMEM;

    if (!config->enabled) {
	config->enabled = true;
	config->next_index = 1;
	device_defaults(config);
    }
    return 0;
}

int netvirt_add_if(Tracee *tracee, const char *spec)
{
    NetVirtConfig *config;
    int status;

    if (spec == NULL || spec[0] == '\0') {
	note(tracee, ERROR, USER, "netvirt: empty interface specification");
	return -EINVAL;
    }

    status = netvirt_enable(tracee);
    if (status < 0)
	return status;
    config = netvirt_get_config(tracee, true);
    if (config == NULL)
	return -ENOMEM;

    return parse_device_spec(tracee, config, spec);
}

int netvirt_add_route(Tracee *tracee, const char *spec)
{
    NetVirtConfig *config;
    int status;

    if (spec == NULL || spec[0] == '\0') {
	note(tracee, ERROR, USER, "netvirt: empty route specification");
	return -EINVAL;
    }

    status = netvirt_enable(tracee);
    if (status < 0)
	return status;
    config = netvirt_get_config(tracee, true);
    if (config == NULL)
	return -ENOMEM;

    return parse_route_spec(tracee, config, spec);
}

int netvirt_set_wg(Tracee *tracee, const char *spec)
{
    NetVirtConfig *config;
    NetVirtDev *dev;
    char *copy;
    char *colon;
    int status;

    if (spec == NULL || spec[0] == '\0') {
	note(tracee, ERROR, USER, "netvirt: empty WireGuard specification");
	return -EINVAL;
    }

    status = netvirt_enable(tracee);
    if (status < 0)
	return status;
    config = netvirt_get_config(tracee, true);
    if (config == NULL)
	return -ENOMEM;

    copy = talloc_strdup(tracee->ctx, spec);
    if (copy == NULL)
	return -ENOMEM;

    colon = strchr(copy, ':');
    if (colon != NULL)
	*colon++ = '\0';

    dev = netvirt_dev_by_name(config, copy);
    if (dev == NULL) {
	note(tracee, ERROR, USER, "netvirt: unknown interface \"%s\"", copy);
	talloc_free(copy);
	return -ENODEV;
    }

    dev->bridge = NETVIRT_BRIDGE_WG_USER;
    if (colon != NULL && colon[0] != '\0') {
	TALLOC_FREE(dev->wg_conf);
	dev->wg_conf = talloc_strdup(config, colon);
	if (dev->wg_conf == NULL) {
	    talloc_free(copy);
	    return -ENOMEM;
	}
    } else {
	const char *environment = getenv("UVROOT_NETVIRT_WG_CONF");

	if (environment != NULL)
	    dev->wg_conf = talloc_strdup(config, environment);
    }

    talloc_free(copy);
    return 0;
}

int netvirt_set_bridge(Tracee *tracee, const char *spec)
{
    NetVirtConfig *config;
    char *copy;
    char *cursor;
    int status;

    if (spec == NULL || spec[0] == '\0') {
	note(tracee, ERROR, USER, "netvirt: empty bridge specification");
	return -EINVAL;
    }

    status = netvirt_enable(tracee);
    if (status < 0)
	return status;
    config = netvirt_get_config(tracee, true);
    if (config == NULL)
	return -ENOMEM;

    copy = talloc_strdup(tracee->ctx, spec);
    if (copy == NULL)
	return -ENOMEM;

    cursor = copy;
    while (cursor != NULL && *cursor != '\0') {
	char *comma = strchr(cursor, ',');
	char *value;

	if (comma != NULL)
	    *comma = '\0';
	value = strchr(cursor, '=');
	if (value != NULL)
	    *value++ = '\0';

	if (strcmp(cursor, "nat") == 0 || strcmp(cursor, "auto") == 0
	    || strcmp(cursor, "none") == 0)
	    config->default_bridge = (strcmp(cursor, "none") == 0)
		? NETVIRT_BRIDGE_NONE
		: ((strcmp(cursor, "nat") == 0)
		   ? NETVIRT_BRIDGE_NAT : NETVIRT_BRIDGE_WG_USER);
	else if (strcmp(cursor, "userspace") == 0
		 || strcmp(cursor, "wireguard") == 0)
	    config->default_bridge = NETVIRT_BRIDGE_WG_USER;
	else if (strcmp(cursor, "kernel") == 0)
	    config->default_bridge = NETVIRT_BRIDGE_WG_KERNEL;
	else if (strcmp(cursor, "exec") == 0 && value != NULL) {
	    config->wg_exec = talloc_strdup(config, value);
	} else if (strcmp(cursor, "lib") == 0 && value != NULL) {
	    config->wg_lib = talloc_strdup(config, value);
	} else {
	    note(tracee, ERROR, USER,
		 "netvirt: unknown bridge option \"%s\"", cursor);
	    talloc_free(copy);
	    return -EINVAL;
	}

	cursor = (comma != NULL) ? comma + 1 : NULL;
    }

    config->bridge_explicit = true;
    talloc_free(copy);
    return 0;
}

int netvirt_finalize(Tracee *tracee)
{
    NetVirtConfig *config = netvirt_get_config(tracee, false);
    int i;

    if (!is_enabled(config))
	return 0;

    /* The bridge is settled once, when every --wg/--net-bridge option
     * is known.  A device without an explicit WireGuard configuration
     * keeps its default (veth0/vtun are bridged, lo is not).  */
    netvirt_bridge_init(tracee, config);

    if (config->default_bridge == NETVIRT_BRIDGE_WG_KERNEL)
	note(tracee, WARNING, USER,
	     "netvirt: the in-kernel WireGuard driver needs CAP_NET_ADMIN in "
	     "the network namespace, which uvroot does not have; using the "
	     "user-space bridge instead");

    for (i = 0; i < config->ndevs; i++) {
	NetVirtDev *dev = &config->devs[i];

	if (dev->bridge == NETVIRT_BRIDGE_NONE)
	    continue;

	if (config->default_bridge == NETVIRT_BRIDGE_NONE) {
	    dev->bridge = NETVIRT_BRIDGE_NONE;
	    dev->bridge_status = talloc_asprintf(config, "none");
	    continue;
	}

	if (config->default_bridge == NETVIRT_BRIDGE_NAT) {
	    dev->bridge = NETVIRT_BRIDGE_NAT;
	    dev->bridge_status = talloc_asprintf(config,
						 "nat (user-mode relay)");
	    continue;
	}

	if (dev->wg_conf == NULL
	    && (strcmp(dev->name, NETVIRT_VETH_NAME) == 0
		|| strcmp(dev->name, NETVIRT_VTUN_NAME) == 0)) {
	    /* No peer configured: the bridge still exists, it simply
	     * carries no tunnel yet.  */
	    dev->bridge_status = talloc_asprintf(config, "%s (unconfigured)",
						 dev->kind);
	    continue;
	}

	if (netvirt_bridge_attach(tracee, config, dev) < 0
	    && netvirt_bridge_start_engine(tracee, config, dev) < 0) {
	    dev->bridge = NETVIRT_BRIDGE_NAT;
	    dev->bridge_status = talloc_asprintf(config, "nat (no WireGuard)");
	    note(tracee, WARNING, USER,
		 "netvirt: %s bridged without WireGuard support, using the "
		 "user-mode NAT path", dev->name);
	}
    }

    for (i = 0; i < config->ndevs; i++) {
	NetVirtDev *dev = &config->devs[i];

	VERBOSE(tracee, 2, "netvirt: %s index %d %s mtu %d%s%s", dev->name,
		dev->index, (dev->flags & IFF_UP) ? "up" : "down", dev->mtu,
		dev->has_mac ? " mac " : "",
		dev->bridge_status != NULL ? dev->bridge_status : "");
    }

    return 0;
}

/*
 * The virtual network is built for the native ABI only: the rtnetlink
 * and ioctl synthesis parse the tracee's struct msghdr/iovec, and a
 * 32-bit guest would silently fall back to the host stack.  Rather than
 * pretending, refuse it loudly.  Runs on every syscall stage, so keep it
 * cheap and report once.
 */
static void reject_foreign_abi(Tracee *tracee, NetVirtConfig *config)
{
    if (config->abi_rejected)
	return;
    if (get_abi(tracee) == ABI_DEFAULT)
	return;

    config->abi_rejected = true;
    note(tracee, ERROR, USER,
	 "netvirt: 32-bit software and libraries are not supported with "
	 "--net: the virtual network covers the native ABI only.  Remove "
	 "--net to run 32-bit guests on the host network stack.");
    fflush(stderr);
    /* Stop every tracee and leave without the talloc leak report that
     * a plain exit() would print.  */
    kill_all_tracees();
    _exit(EXIT_FAILURE);
}

static int netvirt_config_destructor(NetVirtConfig *config)
{
    /* Stop the user-space WireGuard processes of this container.  */
    netvirt_bridge_fini(config);
    return 0;
}

int netvirt_callback(Extension *extension, ExtensionEvent event, intptr_t d1,
		     intptr_t d2 UNUSED)
{
    switch (event) {
    case INITIALIZATION:
	if (extension->config == NULL) {
	    NetVirtConfig *config = talloc_zero(extension, NetVirtConfig);

	    if (config == NULL)
		return -1;
	    config->default_bridge = NETVIRT_BRIDGE_WG_USER;
	    config->nat_fallback = true;
	    config->tun_guest_fd = -1;
	    config->tun_engine_fd = -1;
	    talloc_set_destructor(config, netvirt_config_destructor);
	    extension->config = config;
	}
	extension->filtered_sysnums = netvirt_sysnums;
	return 0;

    case INHERIT_PARENT:{
	Tracee *parent = TRACEE(extension);
	Tracee *child = (Tracee *) d1;
	NetVirtConfig *config = config_of(extension);
	NetVirtTracee *parent_state = tracee_state(config, parent->pid, false);
	NetVirtTracee *child_state;

	if (parent_state == NULL)
	    return 0;

	child_state = tracee_state(config, child->pid, true);
	if (child_state == NULL)
	    return 0;

	/* File descriptors are inherited across fork/clone.  */
	{
	    NetVirtFd *entry;
	    for (entry = parent_state->fds; entry != NULL;
		 entry = entry->next) {
		NetVirtFd *copy = fd_add(child_state, entry->fd);
		if (copy != NULL)
		    copy->rtnl = entry->rtnl;
	    }
	}
	return 0;
    }

    case SYSCALL_ENTER_START:{
	NetVirtConfig *config = config_of(extension);

	if (!is_enabled(config))
	    return 0;
	reject_foreign_abi(TRACEE(extension), config);
	/* The engine is built before the fork so its TUN descriptor is
	 * inherited, but its thread is only started now, once the tracer
	 * process will not fork again (fork(2) in a multithreaded process
	 * is best avoided).  */
	if (config->wg_start_pending) {
	    config->wg_start_pending = false;
	    (void) netvirt_bridge_start_pending(TRACEE(extension), config);
	}
	return enter_syscall(TRACEE(extension), config);
    }

    case SYSCALL_EXIT_START:{
	NetVirtConfig *config = config_of(extension);

	if (!is_enabled(config))
	    return 0;
	return exit_syscall(TRACEE(extension), config);
    }

    case REMOVED:
	return 0;

    case PRINT_CONFIG:{
	NetVirtConfig *config = config_of(extension);
	int i;

	if (!is_enabled(config))
	    return 0;

	for (i = 0; i < config->ndevs; i++) {
	    NetVirtDev *dev = &config->devs[i];
	    int j;

	    note(TRACEE(extension), INFO, USER,
		 "netvirt = %s index %d %s mtu %d", dev->name, dev->index,
		 (dev->flags & IFF_UP) ? "up" : "down", dev->mtu);
	    for (j = 0; j < dev->naddrs; j++) {
		char text[INET6_ADDRSTRLEN];

		if (inet_ntop(dev->addrs[j].family, dev->addrs[j].addr,
			      text, sizeof(text)) == NULL)
		    continue;
		note(TRACEE(extension), INFO, USER, "netvirt = %s/%d on %s",
		     text, dev->addrs[j].prefix, dev->name);
	    }
	}
	return 0;
    }

    default:
	return 0;
    }
}
