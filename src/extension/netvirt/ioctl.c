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
 * Legacy SIOCGIF* ioctls, answered from the virtual model.  `ip` uses
 * rtnetlink, but net-tools' ifconfig, if_nametoindex(3) fallbacks and a
 * lot of old software still go through these, and they must not report
 * the host interfaces.
 */

#include <errno.h>
#include <net/if.h>		/* struct ifreq, IFNAMSIZ, IFF_*, */
#include <netinet/in.h>		/* struct sockaddr_in, */
#include <string.h>		/* memcpy(3), memset(3), strcmp(3), */

#include "attribute.h"
#include "extension/netvirt/netvirt.h"
#include "tracee/mem.h"
#include "tracee/tracee.h"

#ifndef SIOCGIFCONF
#define SIOCGIFCONF	0x8912
#endif
#ifndef SIOCGIFFLAGS
#define SIOCGIFFLAGS	0x8913
#endif
#ifndef SIOCSIFFLAGS
#define SIOCSIFFLAGS	0x8914
#endif
#ifndef SIOCGIFADDR
#define SIOCGIFADDR	0x8915
#endif
#ifndef SIOCSIFADDR
#define SIOCSIFADDR	0x8916
#endif
#ifndef SIOCGIFDSTADDR
#define SIOCGIFDSTADDR	0x8917
#endif
#ifndef SIOCGIFBRDADDR
#define SIOCGIFBRDADDR	0x8919
#endif
#ifndef SIOCGIFNETMASK
#define SIOCGIFNETMASK	0x891b
#endif
#ifndef SIOCSIFNETMASK
#define SIOCSIFNETMASK	0x891c
#endif
#ifndef SIOCGIFMETRIC
#define SIOCGIFMETRIC	0x891d
#endif
#ifndef SIOCGIFMTU
#define SIOCGIFMTU	0x8921
#endif
#ifndef SIOCSIFMTU
#define SIOCSIFMTU	0x8922
#endif
#ifndef SIOCGIFHWADDR
#define SIOCGIFHWADDR	0x8927
#endif
#ifndef SIOCGIFINDEX
#define SIOCGIFINDEX	0x8933
#endif
#ifndef SIOCGIFNAME
#define SIOCGIFNAME	0x8910
#endif
#ifndef SIOCGIFTXQLEN
#define SIOCGIFTXQLEN	0x8942
#endif
#ifndef SIOCSIFTXQLEN
#define SIOCSIFTXQLEN	0x8943
#endif

static void set_ipv4(struct sockaddr *address, const uint8_t bytes[4])
{
    struct sockaddr_in *in = (struct sockaddr_in *) address;

    memset(in, 0, sizeof(*in));
    in->sin_family = AF_INET;
    memcpy(&in->sin_addr, bytes, 4);
}

static void ipv4_mask(uint8_t prefix, uint8_t mask[4])
{
    int i;

    for (i = 0; i < 4; i++) {
	int bits = prefix - i * 8;

	mask[i] = (bits >= 8) ? 0xFF
	    : (bits <= 0 ? 0 : (uint8_t) (0xFF << (8 - bits)));
    }
}

static NetVirtAddr *first_ipv4(NetVirtDev *dev)
{
    int i;

    for (i = 0; i < dev->naddrs; i++) {
	if (dev->addrs[i].family == AF_INET)
	    return &dev->addrs[i];
    }
    return NULL;
}

static int handle_getifconf(Tracee *tracee, NetVirtConfig *config, word_t arg,
			    long *result)
{
    struct ifconf ifc;
    char *buffer;
    int maximum;
    int used = 0;
    int needed = 0;
    int i;

    if (read_data(tracee, &ifc, arg, sizeof(ifc)) < 0)
	return -EFAULT;

    buffer = ifc.ifc_ifcu.ifcu_buf;
    maximum = ifc.ifc_len;

    for (i = 0; i < config->ndevs; i++) {
	NetVirtDev *dev = &config->devs[i];
	NetVirtAddr *addr = first_ipv4(dev);

	if (addr == NULL)
	    continue;

	needed += (int) sizeof(struct ifreq);
	if (buffer == NULL || used + (int) sizeof(struct ifreq) > maximum)
	    continue;

	{
	    struct ifreq request;

	    memset(&request, 0, sizeof(request));
	    snprintf(request.ifr_name, IFNAMSIZ, "%s", dev->name);
	    set_ipv4(&request.ifr_addr, addr->addr);
	    if (write_data(tracee, (word_t) buffer + used, &request,
			   sizeof(request)) < 0)
		return -EFAULT;
	    used += (int) sizeof(request);
	}
    }

    ifc.ifc_len = (buffer == NULL) ? needed : used;
    if (write_data(tracee, arg, &ifc, sizeof(ifc)) < 0)
	return -EFAULT;

    *result = 0;
    return 1;
}

int netvirt_ioctl_request(Tracee *tracee, NetVirtConfig *config,
			  unsigned long request, word_t arg, long *result)
{
    struct ifreq ifr;
    NetVirtDev *dev;

    switch (request) {
    case SIOCGIFCONF:
	return handle_getifconf(tracee, config, arg, result);
    default:
	break;
    }

    if (arg == 0)
	return 0;

    if (read_data(tracee, &ifr, arg, sizeof(ifr)) < 0)
	return -EFAULT;

    /* SIOCGIFNAME takes an index as input, every other request takes a
     * name; the name is not NUL terminated when it is full.  */
    if (request == SIOCGIFNAME) {
	int index;

	memcpy(&index, &ifr.ifr_ifindex, sizeof(index));
	dev = netvirt_dev_by_index(config, index);
	if (dev == NULL) {
	    *result = -ENODEV;
	    return 1;
	}
	memset(ifr.ifr_name, 0, IFNAMSIZ);
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", dev->name);
	if (write_data(tracee, arg, &ifr, sizeof(ifr)) < 0)
	    return -EFAULT;
	*result = 0;
	return 1;
    }

    {
	char name[IFNAMSIZ + 1];

	memcpy(name, ifr.ifr_name, IFNAMSIZ);
	name[IFNAMSIZ] = '\0';
	dev = netvirt_dev_by_name(config, name);
    }

    switch (request) {
    case SIOCGIFFLAGS:
    case SIOCSIFFLAGS:
    case SIOCGIFADDR:
    case SIOCSIFADDR:
    case SIOCGIFDSTADDR:
    case SIOCGIFBRDADDR:
    case SIOCGIFNETMASK:
    case SIOCSIFNETMASK:
    case SIOCGIFMETRIC:
    case SIOCGIFMTU:
    case SIOCSIFMTU:
    case SIOCGIFHWADDR:
    case SIOCGIFINDEX:
    case SIOCGIFTXQLEN:
    case SIOCSIFTXQLEN:
	break;
    default:
	return 0;		/* not ours */
    }

    if (dev == NULL) {
	*result = -ENODEV;
	return 1;
    }

    switch (request) {
    case SIOCGIFFLAGS:
	ifr.ifr_flags = (short) (dev->flags & 0xFFFF);
	break;

    case SIOCSIFFLAGS:
	dev->flags = (dev->flags & ~0xFFFFu)
	    | (unsigned short) ifr.ifr_flags;
	dev->operstate = (dev->flags & IFF_UP)
	    ? NETVIRT_OPER_UP : NETVIRT_OPER_DOWN;
	dev->carrier = (dev->flags & IFF_UP) != 0;
	break;

    case SIOCGIFINDEX:
	ifr.ifr_ifindex = dev->index;
	break;

    case SIOCGIFMTU:
	ifr.ifr_mtu = dev->mtu;
	break;

    case SIOCSIFMTU:
	dev->mtu = ifr.ifr_mtu;
	break;

    case SIOCGIFTXQLEN:
	ifr.ifr_qlen = (int) dev->txqlen;
	break;

    case SIOCSIFTXQLEN:
	dev->txqlen = (unsigned) ifr.ifr_qlen;
	break;

    case SIOCGIFMETRIC:
	ifr.ifr_metric = 0;
	break;

    case SIOCGIFHWADDR:
	memset(&ifr.ifr_hwaddr, 0, sizeof(ifr.ifr_hwaddr));
	ifr.ifr_hwaddr.sa_family = (unsigned short) dev->arphrd;
	if (dev->has_mac)
	    memcpy(ifr.ifr_hwaddr.sa_data, dev->mac, 6);
	break;

    case SIOCGIFADDR:{
	NetVirtAddr *addr = first_ipv4(dev);

	if (addr == NULL) {
	    *result = -EADDRNOTAVAIL;
	    return 1;
	}
	set_ipv4(&ifr.ifr_addr, addr->addr);
	break;
    }

    case SIOCSIFADDR:{
	NetVirtAddr addr;
	struct sockaddr_in *in = (struct sockaddr_in *) &ifr.ifr_addr;

	memset(&addr, 0, sizeof(addr));
	addr.family = AF_INET;
	addr.prefix = 24;
	addr.scope = NETVIRT_SCOPE_UNIVERSE;
	memcpy(addr.addr, &in->sin_addr, 4);
	snprintf(addr.label, sizeof(addr.label), "%s", dev->name);
	if (netvirt_addr_add(dev, &addr) == 0 && config != NULL)
	    (void) netvirt_addr_route(config, dev, &addr, true);
	break;
    }

    case SIOCGIFNETMASK:{
	NetVirtAddr *addr = first_ipv4(dev);
	uint8_t mask[4];

	if (addr == NULL) {
	    *result = -EADDRNOTAVAIL;
	    return 1;
	}
	ipv4_mask(addr->prefix, mask);
	set_ipv4(&ifr.ifr_netmask, mask);
	break;
    }

    case SIOCSIFNETMASK:{
	NetVirtAddr *addr = first_ipv4(dev);
	struct sockaddr_in *in = (struct sockaddr_in *) &ifr.ifr_netmask;
	uint32_t mask;

	if (addr == NULL) {
	    *result = -EADDRNOTAVAIL;
	    return 1;
	}
	memcpy(&mask, &in->sin_addr, 4);
	mask = ntohl(mask);
	{
	    int prefix = 0;

	    while (prefix < 32 && (mask & 0x80000000u) != 0) {
		prefix++;
		mask <<= 1;
	    }
	    (void) netvirt_addr_route(config, dev, addr, false);
	    addr->prefix = (uint8_t) prefix;
	    (void) netvirt_addr_route(config, dev, addr, true);
	}
	break;
    }

    case SIOCGIFBRDADDR:{
	NetVirtAddr *addr = first_ipv4(dev);

	if (addr == NULL) {
	    *result = -EADDRNOTAVAIL;
	    return 1;
	}
	if (addr->has_broadcast)
	    set_ipv4(&ifr.ifr_broadaddr, addr->broadcast);
	else
	    set_ipv4(&ifr.ifr_broadaddr, addr->addr);
	break;
    }

    case SIOCGIFDSTADDR:{
	uint8_t none[4] = { 0, 0, 0, 0 };
	set_ipv4(&ifr.ifr_dstaddr, none);
	break;
    }

    default:
	return 0;
    }

    if (write_data(tracee, arg, &ifr, sizeof(ifr)) < 0)
	return -EFAULT;
    *result = 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Virtual /dev/net/tun                                                */
/* ------------------------------------------------------------------ */

#ifndef TUNSETIFF
#define TUNSETIFF	0x400454ca
#endif
#ifndef TUNGETIFF
#define TUNGETIFF	0x800454d2
#endif
#ifndef TUNGETFEATURES
#define TUNGETFEATURES	0x800454cf
#endif
#ifndef TUNSETPERSIST
#define TUNSETPERSIST	0x400454cb
#endif
#ifndef TUNSETOWNER
#define TUNSETOWNER	0x400454cc
#endif
#ifndef TUNSETGROUP
#define TUNSETGROUP	0x400454ce
#endif
#ifndef TUNSETOFFLOAD
#define TUNSETOFFLOAD	0x400454d0
#endif
#ifndef TUNSETVNETHDRSZ
#define TUNSETVNETHDRSZ	0x400454d8
#endif
#ifndef TUNSETQUEUE
#define TUNSETQUEUE	0x400454d9
#endif

#define TUN_IFF_TUN		0x0001
#define TUN_IFF_TAP		0x0002
#define TUN_IFF_MULTI_QUEUE	0x0100
#define TUN_IFF_NO_PI		0x1000
#define TUN_IFF_ONE_QUEUE	0x2000
#define TUN_IFF_VNET_HDR	0x4000

int netvirt_tun_ioctl(Tracee *tracee, NetVirtConfig *config,
		      NetVirtFd *entry, unsigned long request,
		      word_t argument, long *result)
{
    struct ifreq ifr;
    NetVirtDev *dev;

    switch (request) {
    case TUNSETIFF:{
	char name[IFNAMSIZ + 1];

	if (read_data(tracee, &ifr, argument, sizeof(ifr)) < 0)
	    return -EFAULT;
	memcpy(name, ifr.ifr_name, IFNAMSIZ);
	name[IFNAMSIZ] = '\0';
	if (name[0] == '\0') {
	    *result = -EINVAL;
	    return 1;
	}

	dev = netvirt_dev_by_name(config, name);
	if (dev == NULL) {
	    dev = netvirt_dev_alloc(config, name, "tun");
	    if (dev == NULL) {
		*result = -ENOSPC;
		return 1;
	    }
	}

	if (ifr.ifr_flags & TUN_IFF_TAP) {
	    snprintf(dev->kind, sizeof(dev->kind), "tap");
	    dev->arphrd = NETVIRT_ARPHRD_ETHER;
	} else if (ifr.ifr_flags & TUN_IFF_TUN) {
	    snprintf(dev->kind, sizeof(dev->kind), "tun");
	    dev->arphrd = NETVIRT_ARPHRD_NONE;
	    dev->flags |= IFF_POINTOPOINT | IFF_NOARP;
	} else {
	    *result = -EINVAL;
	    return 1;
	}

	snprintf(entry->tun_name, sizeof(entry->tun_name), "%s", name);
	if (write_data(tracee, argument, &ifr, sizeof(ifr)) < 0)
	    return -EFAULT;
	*result = 0;
	return 1;
    }

    case TUNGETIFF:
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, IFNAMSIZ, "%.*s", IFNAMSIZ - 1,
		 entry->tun_name[0] != '\0' ? entry->tun_name : "tun0");
	ifr.ifr_flags = TUN_IFF_TUN | TUN_IFF_NO_PI;
	if (write_data(tracee, argument, &ifr, sizeof(ifr)) < 0)
	    return -EFAULT;
	*result = 0;
	return 1;

    case TUNGETFEATURES:{
	unsigned int features = TUN_IFF_TUN | TUN_IFF_TAP | TUN_IFF_NO_PI
	    | TUN_IFF_ONE_QUEUE | TUN_IFF_VNET_HDR | TUN_IFF_MULTI_QUEUE;

	if (write_data(tracee, argument, &features, sizeof(features)) < 0)
	    return -EFAULT;
	*result = 0;
	return 1;
    }

    case TUNSETPERSIST:
    case TUNSETOWNER:
    case TUNSETGROUP:
    case TUNSETOFFLOAD:
    case TUNSETVNETHDRSZ:
    case TUNSETQUEUE:
	*result = 0;
	return 1;

    default:
	return 0;
    }
}
