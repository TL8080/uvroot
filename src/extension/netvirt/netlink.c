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
 * rtnetlink synthesis.  The container's `ip` (and getifaddrs(3), and
 * anything else speaking NETLINK_ROUTE) is answered from the virtual
 * device/address/route model: dumps are turned into RTM_NEW* multipart
 * replies followed by NLMSG_DONE, and configuration requests are applied
 * to the model and acknowledged.  Nothing reaches the host stack, so the
 * container never sees -- nor changes -- the host interfaces.
 */

#include <arpa/inet.h>		/* inet_pton(3), */
#include <errno.h>		/* E*, */
#include <net/if.h>		/* IFF_*, */
#include <netinet/in.h>		/* AF_INET, */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>		/* memcpy(3), memset(3), strcmp(3), */
#include <sys/socket.h>		/* AF_*, */

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/if_addr.h>
#include <linux/if_arp.h>

#include "attribute.h"
#include "cli/note.h"
#include "extension/netvirt/netvirt.h"
#include "tracee/tracee.h"

#define NETVIRT_MSG_BUFFER 2048

typedef struct {
    unsigned char buffer[NETVIRT_MSG_BUFFER];
    struct nlmsghdr *header;
} Msg;

/* ------------------------------------------------------------------ */
/* Message builder                                                     */
/* ------------------------------------------------------------------ */

static void msg_begin(Msg *msg, uint16_t type, uint16_t flags, uint32_t seq,
		      const void *payload, size_t payload_length)
{
    memset(msg->buffer, 0, sizeof(msg->buffer));
    msg->header = (struct nlmsghdr *) msg->buffer;
    msg->header->nlmsg_len = NLMSG_HDRLEN + payload_length;
    msg->header->nlmsg_type = type;
    msg->header->nlmsg_flags = flags;
    msg->header->nlmsg_seq = seq;
    msg->header->nlmsg_pid = 0;

    if (payload != NULL && payload_length > 0)
	memcpy(NLMSG_DATA(msg->header), payload, payload_length);
}

static void msg_attr(Msg *msg, uint16_t type, const void *data, size_t length)
{
    size_t offset = NLMSG_ALIGN(msg->header->nlmsg_len);
    size_t total = RTA_ALIGN(RTA_LENGTH(length));
    struct rtattr *rta;

    if (offset + total > sizeof(msg->buffer))
	return;

    rta = (struct rtattr *) (msg->buffer + offset);
    rta->rta_type = type;
    rta->rta_len = RTA_LENGTH(length);
    if (data != NULL && length > 0)
	memcpy(RTA_DATA(rta), data, length);
    memset((unsigned char *) rta + RTA_LENGTH(length), 0,
	   total - RTA_LENGTH(length));
    msg->header->nlmsg_len = offset + total;
}

static void msg_attr_u8(Msg *msg, uint16_t type, uint8_t value)
{
    msg_attr(msg, type, &value, sizeof(value));
}

static void msg_attr_u32(Msg *msg, uint16_t type, uint32_t value)
{
    msg_attr(msg, type, &value, sizeof(value));
}

static size_t msg_nest_start(Msg *msg, uint16_t type)
{
    size_t offset = NLMSG_ALIGN(msg->header->nlmsg_len);
    struct rtattr *rta;

    if (offset + RTA_LENGTH(0) > sizeof(msg->buffer))
	return offset;

    rta = (struct rtattr *) (msg->buffer + offset);
    rta->rta_type = type;
    rta->rta_len = RTA_LENGTH(0);
    msg->header->nlmsg_len = offset + RTA_LENGTH(0);
    return offset;
}

static void msg_nest_end(Msg *msg, size_t offset)
{
    struct rtattr *rta = (struct rtattr *) (msg->buffer + offset);

    rta->rta_len = (uint16_t) (msg->header->nlmsg_len - offset);
}

static void emit(NetVirtFd *fd, const Msg *msg)
{
    struct nlmsghdr *header = (struct nlmsghdr *) msg->buffer;

    /* ip(8) discards every reply whose nlmsg_pid is not the local port
     * id of its socket (see rtnl_dump_filter in libnetlink).  The
     * kernel echoes the destination port here, so do the same.  */
    header->nlmsg_pid = fd->local_pid;
    netvirt_fd_append(fd, msg->buffer, header->nlmsg_len);
}

static void emit_done(NetVirtFd *fd, uint32_t seq)
{
    Msg msg;
    /* Recent kernels put the dump status in a 4-byte payload and
     * iproute2 refuses a header-only DONE ("DONE truncated").  */
    int error = 0;

    msg_begin(&msg, NLMSG_DONE, 0, seq, &error, sizeof(error));
    emit(fd, &msg);
}

static void emit_ack(NetVirtFd *fd, const struct nlmsghdr *request, int error)
{
    struct {
	int error;
	struct nlmsghdr msg;
    } payload;
    Msg msg;

    memset(&payload, 0, sizeof(payload));
    payload.error = error;
    memcpy(&payload.msg, request, sizeof(struct nlmsghdr));
    msg_begin(&msg, NLMSG_ERROR, 0, request->nlmsg_seq, &payload,
	      sizeof(payload));
    emit(fd, &msg);
}

/* The kernel only answers a change request with an ACK when NLM_F_ACK
 * was asked for; an unsolicited ACK makes iproute2's rtnl_send_check()
 * report "Failed to send flush request: Success".  Errors are always
 * reported.  */
static void emit_status(NetVirtFd *fd, const struct nlmsghdr *request,
			int error)
{
    if (error != 0 || (request->nlmsg_flags & NLM_F_ACK) != 0)
	emit_ack(fd, request, error);
}

/* ------------------------------------------------------------------ */
/* Attribute helpers                                                   */
/* ------------------------------------------------------------------ */

static struct rtattr *attrs_after(const struct nlmsghdr *header, size_t size)
{
    return (struct rtattr *) ((char *) NLMSG_DATA(header)
			      + NLMSG_ALIGN(size));
}

static int attrs_length(const struct nlmsghdr *header, size_t size)
{
    int length = (int) header->nlmsg_len - (int) NLMSG_HDRLEN
	- (int) NLMSG_ALIGN(size);

    return length > 0 ? length : 0;
}

static struct rtattr *attr_find(struct rtattr *rta, int length, int type)
{
    for (; RTA_OK(rta, length); rta = RTA_NEXT(rta, length)) {
	if (rta->rta_type == type)
	    return rta;
    }
    return NULL;
}

static bool attr_u32(struct rtattr *rta, uint32_t *value)
{
    if (rta == NULL || RTA_PAYLOAD(rta) < (int) sizeof(uint32_t))
	return false;
    memcpy(value, RTA_DATA(rta), sizeof(*value));
    return true;
}

static const char *attr_string(struct rtattr *rta)
{
    if (rta == NULL || RTA_PAYLOAD(rta) <= 0)
	return NULL;
    return (const char *) RTA_DATA(rta);
}

/* ------------------------------------------------------------------ */
/* Reply emitters                                                      */
/* ------------------------------------------------------------------ */

static void emit_link(NetVirtFd *fd, const NetVirtDev *dev, uint16_t flags,
		      uint32_t seq)
{
    struct ifinfomsg info;
    uint32_t value;
    Msg msg;
    size_t nest;

    memset(&info, 0, sizeof(info));
    info.ifi_family = AF_UNSPEC;
    info.ifi_type = (uint16_t) dev->arphrd;
    info.ifi_index = dev->index;
    info.ifi_flags = dev->flags;
    info.ifi_change = 0;

    msg_begin(&msg, RTM_NEWLINK, flags, seq, &info, sizeof(info));

    if (dev->has_mac)
	msg_attr(&msg, IFLA_ADDRESS, dev->mac, sizeof(dev->mac));
    if (dev->arphrd == ARPHRD_ETHER) {
	static const uint8_t broadcast[6] =
	    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	msg_attr(&msg, IFLA_BROADCAST, broadcast, sizeof(broadcast));
    }
    msg_attr(&msg, IFLA_IFNAME, dev->name, strlen(dev->name) + 1);
    value = (uint32_t) dev->mtu;
    msg_attr(&msg, IFLA_MTU, &value, sizeof(value));
    value = dev->txqlen;
    msg_attr(&msg, IFLA_TXQLEN, &value, sizeof(value));
    msg_attr_u8(&msg, IFLA_OPERSTATE, (uint8_t) dev->operstate);
    msg_attr_u8(&msg, IFLA_LINKMODE, 0);
    msg_attr_u32(&msg, IFLA_GROUP, dev->group);
    msg_attr_u32(&msg, IFLA_NUM_TX_QUEUES, 1);
    msg_attr_u32(&msg, IFLA_NUM_RX_QUEUES, 1);
    msg_attr_u8(&msg, IFLA_CARRIER, dev->carrier ? 1 : 0);
    msg_attr(&msg, IFLA_QDISC, "noqueue", sizeof("noqueue"));

    nest = msg_nest_start(&msg, IFLA_LINKINFO);
    msg_attr(&msg, IFLA_INFO_KIND, dev->kind, strlen(dev->kind) + 1);
    msg_nest_end(&msg, nest);

    emit(fd, &msg);
}

static void emit_addr(NetVirtFd *fd, const NetVirtDev *dev,
		      const NetVirtAddr *addr, uint16_t flags, uint32_t seq)
{
    struct ifaddrmsg info;
    uint32_t ifa_flags = IFA_F_PERMANENT;
    size_t length = (addr->family == AF_INET) ? 4 : 16;
    Msg msg;

    memset(&info, 0, sizeof(info));
    info.ifa_family = (uint8_t) addr->family;
    info.ifa_prefixlen = addr->prefix;
    info.ifa_flags = (uint8_t) ifa_flags;
    info.ifa_scope = addr->scope;
    info.ifa_index = (uint32_t) dev->index;

    msg_begin(&msg, RTM_NEWADDR, flags, seq, &info, sizeof(info));
    msg_attr(&msg, IFA_ADDRESS, addr->addr, length);
    msg_attr(&msg, IFA_LOCAL, addr->addr, length);
    if (addr->has_broadcast)
	msg_attr(&msg, IFA_BROADCAST, addr->broadcast, 4);
    msg_attr(&msg, IFA_LABEL, addr->label, strlen(addr->label) + 1);
    msg_attr_u32(&msg, IFA_FLAGS, ifa_flags);
    if (addr->family == AF_INET6) {
	struct ifa_cacheinfo cache;

	memset(&cache, 0, sizeof(cache));
	cache.ifa_prefered = 0xFFFFFFFF;
	cache.ifa_valid = 0xFFFFFFFF;
	msg_attr(&msg, IFA_CACHEINFO, &cache, sizeof(cache));
    }
    emit(fd, &msg);
}

static void emit_route(NetVirtFd *fd, const NetVirtRoute *route, uint16_t flags,
		       uint32_t seq)
{
    struct rtmsg rtm;
    size_t length = (route->family == AF_INET) ? 4 : 16;
    Msg msg;

    memset(&rtm, 0, sizeof(rtm));
    rtm.rtm_family = (uint8_t) route->family;
    rtm.rtm_dst_len = route->prefix;
    rtm.rtm_table = (route->table < 256) ? (uint8_t) route->table
	: (uint8_t) RT_TABLE_UNSPEC;
    rtm.rtm_protocol = (uint8_t) route->protocol;
    rtm.rtm_scope = (uint8_t) route->scope;
    rtm.rtm_type = (uint8_t) route->type;
    rtm.rtm_flags = route->flags;

    msg_begin(&msg, RTM_NEWROUTE, flags, seq, &rtm, sizeof(rtm));
    if (route->prefix > 0)
	msg_attr(&msg, RTA_DST, route->dst, length);
    if (route->has_gw)
	msg_attr(&msg, RTA_GATEWAY, route->gw, length);
    if (route->oif != 0)
	msg_attr_u32(&msg, RTA_OIF, (uint32_t) route->oif);
    if (route->metric != 0)
	msg_attr_u32(&msg, RTA_PRIORITY, route->metric);
    if (route->table >= 256)
	msg_attr_u32(&msg, RTA_TABLE, (uint32_t) route->table);
    emit(fd, &msg);
}

/* ------------------------------------------------------------------ */
/* Request handlers                                                    */
/* ------------------------------------------------------------------ */

static void handle_getlink(NetVirtConfig *config, NetVirtFd *fd,
			   struct nlmsghdr *header)
{
    struct ifinfomsg *info = NLMSG_DATA(header);
    struct rtattr *rta = attrs_after(header, sizeof(*info));
    int length = attrs_length(header, sizeof(*info));

    if (header->nlmsg_flags & NLM_F_DUMP) {
	int i;

	for (i = 0; i < config->ndevs; i++)
	    emit_link(fd, &config->devs[i], NLM_F_MULTI, header->nlmsg_seq);
	emit_done(fd, header->nlmsg_seq);
	return;
    }

    {
	NetVirtDev *dev = NULL;

	if (info->ifi_index != 0)
	    dev = netvirt_dev_by_index(config, info->ifi_index);
	if (dev == NULL) {
	    const char *name = attr_string(attr_find(rta, length, IFLA_IFNAME));

	    if (name != NULL)
		dev = netvirt_dev_by_name(config, name);
	}
	if (dev == NULL)
	    emit_status(fd, header, -ENODEV);
	else
	    emit_link(fd, dev, 0, header->nlmsg_seq);
    }
}

static const char *linkinfo_kind(struct rtattr *rta, int length)
{
    struct rtattr *linkinfo = attr_find(rta, length, IFLA_LINKINFO);
    struct rtattr *kind;

    if (linkinfo == NULL)
	return NULL;
    kind = attr_find(RTA_DATA(linkinfo), RTA_PAYLOAD(linkinfo),
		     IFLA_INFO_KIND);
    return attr_string(kind);
}

static void handle_setlink(NetVirtConfig *config, NetVirtFd *fd,
			   struct nlmsghdr *header)
{
    struct ifinfomsg *info = NLMSG_DATA(header);
    struct rtattr *rta = attrs_after(header, sizeof(*info));
    int length = attrs_length(header, sizeof(*info));
    NetVirtDev *dev = NULL;
    const char *name;
    bool created = false;

    if (info->ifi_index != 0)
	dev = netvirt_dev_by_index(config, info->ifi_index);

    name = attr_string(attr_find(rta, length, IFLA_IFNAME));
    if (dev == NULL && name != NULL)
	dev = netvirt_dev_by_name(config, name);

    if (dev == NULL && (header->nlmsg_flags & NLM_F_CREATE) != 0) {
	const char *kind = linkinfo_kind(rta, length);
	const char *wanted = (name != NULL) ? name : NULL;

	if (wanted == NULL || wanted[0] == '\0') {
	    emit_status(fd, header, -EINVAL);
	    return;
	}
	dev = netvirt_dev_alloc(config, wanted,
				kind != NULL ? kind : "dummy");
	if (dev == NULL) {
	    emit_status(fd, header, -ENOSPC);
	    return;
	}
	dev->flags = (info->ifi_flags != 0)
	    ? (info->ifi_flags | IFF_RUNNING)
	    : (IFF_UP | IFF_RUNNING | IFF_BROADCAST | IFF_MULTICAST);
	dev->operstate = NETVIRT_OPER_UP;
	dev->carrier = true;
	dev->mtu = NETVIRT_MTU_DEFAULT;
	if (strcmp(dev->kind, "tun") == 0) {
	    dev->arphrd = ARPHRD_NONE;
	    dev->flags |= IFF_POINTOPOINT | IFF_NOARP;
	    dev->mtu = 1420;
	} else {
	    dev->arphrd = ARPHRD_ETHER;
	    dev->has_mac = true;
	    dev->mac[0] = 0x02;
	    dev->mac[1] = 0x00;
	    dev->mac[5] = (uint8_t) dev->index;
	}
	if (strcmp(dev->kind, "wireguard") == 0)
	    dev->bridge = config->default_bridge;
	created = true;
    }

    if (dev == NULL) {
	emit_status(fd, header, (header->nlmsg_flags & NLM_F_EXCL) != 0
		 ? -EEXIST : -ENODEV);
	return;
    }

    /* Flags: ifi_change is the mask of the bits the caller controls;
     * when it is zero the request only changes attributes (MTU, MAC,
     * name), exactly like the kernel.  */
    if (!created && info->ifi_change != 0) {
	uint32_t change = info->ifi_change;

	dev->flags = (dev->flags & ~change) | (info->ifi_flags & change);
	dev->operstate = (dev->flags & IFF_UP)
	    ? NETVIRT_OPER_UP : NETVIRT_OPER_DOWN;
	dev->carrier = (dev->flags & IFF_UP) != 0;
    }

    if (name != NULL && strcmp(name, dev->name) != 0) {
	snprintf(dev->name, sizeof(dev->name), "%s", name);
	netvirt_route_reindex(config);
    }

    {
	struct rtattr *mtu = attr_find(rta, length, IFLA_MTU);
	struct rtattr *txqlen = attr_find(rta, length, IFLA_TXQLEN);
	struct rtattr *address = attr_find(rta, length, IFLA_ADDRESS);
	uint32_t value;

	if (attr_u32(mtu, &value))
	    dev->mtu = (int) value;
	if (attr_u32(txqlen, &value))
	    dev->txqlen = value;
	if (address != NULL && RTA_PAYLOAD(address) == 6) {
	    memcpy(dev->mac, RTA_DATA(address), 6);
	    dev->has_mac = true;
	}
    }

    emit_status(fd, header, 0);
}

static void handle_dellink(NetVirtConfig *config, NetVirtFd *fd,
			   struct nlmsghdr *header)
{
    struct ifinfomsg *info = NLMSG_DATA(header);
    struct rtattr *rta = attrs_after(header, sizeof(*info));
    int length = attrs_length(header, sizeof(*info));
    NetVirtDev *dev = NULL;

    if (info->ifi_index != 0)
	dev = netvirt_dev_by_index(config, info->ifi_index);
    if (dev == NULL) {
	const char *name = attr_string(attr_find(rta, length, IFLA_IFNAME));

	if (name != NULL)
	    dev = netvirt_dev_by_name(config, name);
    }

    if (dev == NULL) {
	emit_status(fd, header, -ENODEV);
	return;
    }
    if (strcmp(dev->name, "lo") == 0) {
	emit_status(fd, header, -EPERM);
	return;
    }

    netvirt_dev_remove(config, dev);
    emit_status(fd, header, 0);
}

static void handle_getaddr(NetVirtConfig *config, NetVirtFd *fd,
			   struct nlmsghdr *header)
{
    struct ifaddrmsg *info = NLMSG_DATA(header);
    int i;

    for (i = 0; i < config->ndevs; i++) {
	NetVirtDev *dev = &config->devs[i];
	int j;

	if (info->ifa_index != 0 && (int) info->ifa_index != dev->index)
	    continue;

	for (j = 0; j < dev->naddrs; j++) {
	    if (info->ifa_family != AF_UNSPEC
		&& info->ifa_family != dev->addrs[j].family)
		continue;
	    emit_addr(fd, dev, &dev->addrs[j], NLM_F_MULTI,
		      header->nlmsg_seq);
	}
    }
    emit_done(fd, header->nlmsg_seq);
}

static void handle_setaddr(NetVirtConfig *config, NetVirtFd *fd,
			   struct nlmsghdr *header)
{
    struct ifaddrmsg *info = NLMSG_DATA(header);
    struct rtattr *rta = attrs_after(header, sizeof(*info));
    int length = attrs_length(header, sizeof(*info));
    struct rtattr *local = attr_find(rta, length, IFA_LOCAL);
    struct rtattr *address = attr_find(rta, length, IFA_ADDRESS);
    struct rtattr *broadcast = attr_find(rta, length, IFA_BROADCAST);
    struct rtattr *label = attr_find(rta, length, IFA_LABEL);
    NetVirtDev *dev;
    NetVirtAddr addr;
    uint8_t data[16];
    size_t size;

    dev = netvirt_dev_by_index(config, (int) info->ifa_index);
    if (dev == NULL) {
	emit_status(fd, header, -ENODEV);
	return;
    }

    if (info->ifa_family != AF_INET && info->ifa_family != AF_INET6) {
	emit_status(fd, header, -EAFNOSUPPORT);
	return;
    }
    size = (info->ifa_family == AF_INET) ? 4 : 16;

    if (local == NULL)
	local = address;
    if (local == NULL || (size_t) RTA_PAYLOAD(local) < size) {
	emit_status(fd, header, -EINVAL);
	return;
    }

    memset(&addr, 0, sizeof(addr));
    addr.family = info->ifa_family;
    addr.prefix = info->ifa_prefixlen;
    addr.scope = info->ifa_scope;
    memcpy(data, RTA_DATA(local), size);
    memcpy(addr.addr, data, size);
    if (broadcast != NULL && RTA_PAYLOAD(broadcast) >= 4) {
	memcpy(addr.broadcast, RTA_DATA(broadcast), 4);
	addr.has_broadcast = true;
    }
    if (label != NULL) {
	const char *text = attr_string(label);

	if (text != NULL)
	    snprintf(addr.label, sizeof(addr.label), "%s", text);
    }
    if (addr.label[0] == '\0')
	snprintf(addr.label, sizeof(addr.label), "%s", dev->name);

    if (header->nlmsg_type == RTM_DELADDR) {
	if (netvirt_addr_del(dev, addr.family, addr.addr, addr.prefix) < 0) {
	    emit_status(fd, header, -EADDRNOTAVAIL);
	    return;
	}
	(void) netvirt_addr_route(config, dev, &addr, false);
	emit_status(fd, header, 0);
	return;
    }

    if (netvirt_addr_add(dev, &addr) == -EEXIST) {
	if ((header->nlmsg_flags & NLM_F_EXCL) != 0) {
	    emit_status(fd, header, -EEXIST);
	    return;
	}
	emit_status(fd, header, 0);
	return;
    }
    (void) netvirt_addr_route(config, dev, &addr, true);
    emit_status(fd, header, 0);
}

static const NetVirtRoute *route_lookup(NetVirtConfig *config, int family,
					const uint8_t *dst)
{
    const NetVirtRoute *best = NULL;
    int i;

    for (i = 0; i < config->nroutes; i++) {
	const NetVirtRoute *route = &config->routes[i];
	int bits;

	if (route->family != family)
	    continue;
	if (route->table != NETVIRT_TABLE_MAIN
	    && route->table != NETVIRT_TABLE_UNSPEC)
	    continue;

	if (route->prefix > 0) {
	    /* Prefix match, bit by bit.  */
	    for (bits = 0; bits < route->prefix; bits++) {
		int byte = bits / 8;
		int bit = 7 - (bits % 8);

		if (((route->dst[byte] >> bit) & 1)
		    != ((dst[byte] >> bit) & 1))
		    break;
	    }
	    if (bits < route->prefix)
		continue;
	}

	if (best == NULL || route->prefix > best->prefix
	    || (route->prefix == best->prefix
		&& route->metric < best->metric))
	    best = route;
    }
    return best;
}

static void handle_getroute(NetVirtConfig *config, NetVirtFd *fd,
			    struct nlmsghdr *header)
{
    struct rtmsg *rtm = NLMSG_DATA(header);
    struct rtattr *rta = attrs_after(header, sizeof(*rtm));
    int length = attrs_length(header, sizeof(*rtm));

    if (header->nlmsg_flags & NLM_F_DUMP) {
	int i;

	for (i = 0; i < config->nroutes; i++) {
	    NetVirtRoute *route = &config->routes[i];

	    if (rtm->rtm_family != AF_UNSPEC
		&& rtm->rtm_family != route->family)
		continue;
	    emit_route(fd, route, NLM_F_MULTI, header->nlmsg_seq);
	}
	emit_done(fd, header->nlmsg_seq);
	return;
    }

    {
	struct rtattr *dst = attr_find(rta, length, RTA_DST);
	uint8_t target[16];
	int family = (rtm->rtm_family == AF_UNSPEC)
	    ? AF_INET : rtm->rtm_family;
	const NetVirtRoute *best;

	memset(target, 0, sizeof(target));
	if (dst != NULL)
	    memcpy(target, RTA_DATA(dst),
		   RTA_PAYLOAD(dst) > 16 ? 16 : (size_t) RTA_PAYLOAD(dst));

	best = route_lookup(config, family, target);
	if (best == NULL)
	    emit_status(fd, header, -ENETUNREACH);
	else
	    emit_route(fd, best, 0, header->nlmsg_seq);
    }
}

static void handle_setroute(NetVirtConfig *config, NetVirtFd *fd,
			    struct nlmsghdr *header)
{
    struct rtmsg *rtm = NLMSG_DATA(header);
    struct rtattr *rta = attrs_after(header, sizeof(*rtm));
    int length = attrs_length(header, sizeof(*rtm));
    struct rtattr *dst = attr_find(rta, length, RTA_DST);
    struct rtattr *gateway = attr_find(rta, length, RTA_GATEWAY);
    struct rtattr *oif = attr_find(rta, length, RTA_OIF);
    struct rtattr *priority = attr_find(rta, length, RTA_PRIORITY);
    struct rtattr *table = attr_find(rta, length, RTA_TABLE);
    NetVirtRoute route;
    size_t size = (rtm->rtm_family == AF_INET6) ? 16 : 4;
    uint32_t value;
    int status;

    if (rtm->rtm_family != AF_INET && rtm->rtm_family != AF_INET6) {
	emit_status(fd, header, -EAFNOSUPPORT);
	return;
    }

    memset(&route, 0, sizeof(route));
    route.family = rtm->rtm_family;
    route.prefix = rtm->rtm_dst_len;
    route.protocol = (rtm->rtm_protocol != 0)
	? rtm->rtm_protocol : NETVIRT_PROTO_BOOT;
    route.scope = rtm->rtm_scope;
    route.type = (rtm->rtm_type != 0) ? rtm->rtm_type : NETVIRT_RTN_UNICAST;
    route.table = (rtm->rtm_table != RT_TABLE_UNSPEC)
	? rtm->rtm_table : NETVIRT_TABLE_MAIN;

    if (dst != NULL)
	memcpy(route.dst, RTA_DATA(dst),
	       (size_t) RTA_PAYLOAD(dst) < size ? (size_t) RTA_PAYLOAD(dst)
	       : size);
    if (gateway != NULL) {
	memcpy(route.gw, RTA_DATA(gateway),
	       (size_t) RTA_PAYLOAD(gateway) < size
	       ? (size_t) RTA_PAYLOAD(gateway) : size);
	route.has_gw = true;
    }
    if (attr_u32(oif, &value)) {
	NetVirtDev *dev = netvirt_dev_by_index(config, (int) value);

	route.oif = (int) value;
	if (dev != NULL)
	    snprintf(route.oif_name, sizeof(route.oif_name), "%s",
		     dev->name);
    }
    if (attr_u32(priority, &value))
	route.metric = value;
    if (attr_u32(table, &value))
	route.table = (int) value;

    if (header->nlmsg_type == RTM_DELROUTE) {
	status = netvirt_route_del(config, &route);
	if (status < 0) {
	    /* `ip route flush` deletes routes it just dumped; report
	     * success when nothing matched so that it does not stop.  */
	    emit_status(fd, header, 0);
	    return;
	}
	emit_status(fd, header, 0);
	return;
    }

    if (route.prefix == 0 && !route.has_gw && route.oif == 0) {
	emit_status(fd, header, -EINVAL);
	return;
    }

    status = netvirt_route_add(config, &route);
    emit_status(fd, header, status < 0 ? status : 0);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int netvirt_netlink_request(Tracee *tracee UNUSED, NetVirtConfig *config,
			    NetVirtFd *fd, const unsigned char *request,
			    size_t length)
{
    struct nlmsghdr *header;
    int remaining = (int) length;

    for (header = (struct nlmsghdr *) request;
	 NLMSG_OK(header, remaining);
	 header = NLMSG_NEXT(header, remaining)) {

	switch (header->nlmsg_type) {
	case NLMSG_NOOP:
	case NLMSG_DONE:
	    break;

	case RTM_GETLINK:
	    handle_getlink(config, fd, header);
	    break;

	case RTM_NEWLINK:
	case RTM_SETLINK:
	    handle_setlink(config, fd, header);
	    break;

	case RTM_DELLINK:
	    handle_dellink(config, fd, header);
	    break;

	case RTM_GETADDR:
	    handle_getaddr(config, fd, header);
	    break;

	case RTM_NEWADDR:
	case RTM_DELADDR:
	    handle_setaddr(config, fd, header);
	    break;

	case RTM_GETROUTE:
	    handle_getroute(config, fd, header);
	    break;

	case RTM_NEWROUTE:
	case RTM_DELROUTE:
	    handle_setroute(config, fd, header);
	    break;

	default:
	    /* Every other rtnetlink GET is a dump with no virtual
	     * counterpart: answer with an empty one.  */
	    if (header->nlmsg_type >= RTM_BASE
		&& (header->nlmsg_type & 3) == 2
		&& (header->nlmsg_flags & NLM_F_DUMP) != 0)
		emit_done(fd, header->nlmsg_seq);
	    else
		emit_status(fd, header, -EOPNOTSUPP);
	    break;
	}
    }

    return 0;
}
