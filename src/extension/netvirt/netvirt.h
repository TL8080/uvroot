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

#ifndef NETVIRT_H
#define NETVIRT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>		/* pid_t, */
#include <net/if.h>		/* IFNAMSIZ, IFF_*, */

#include "extension/extension.h"
#include "tracee/tracee.h"

/*
 * The netvirt extension gives an unprivileged container its own virtual
 * network.  It is built the same way the netfs backends are: nothing is
 * created at the kernel level, so it works where uvroot works (unrooted
 * Android/Termux included).
 *
 * Two layers are involved:
 *
 *   control plane   uvroot answers the rtnetlink traffic (ip link, ip
 *                   addr, ip route, getifaddrs) and the legacy SIOCGIF*
 *                   ioctls from an in-process model of virtual devices,
 *                   addresses and routes.  Every change the container
 *                   asks for is applied to that model, so `ip` behaves
 *                   like the container owns a real network stack.
 *
 *   data plane      each device is bound to a bridge.  A WireGuard
 *                   bridge is the default for veth0 (ordinary I/O) and
 *                   vtun (tailscale / intranet).  The bridge is either a
 *                   user-space WireGuard implementation reached through
 *                   its UAPI socket with a socketpair standing in for
 *                   the TUN device (nothing privileged), or -- when no
 *                   such implementation is available -- a user-mode NAT
 *                   that relays through the host stack.
 */

#define NETVIRT_NAME_MAX	IFNAMSIZ	/* 16, NUL included */
#define NETVIRT_MAX_DEVS	32
#define NETVIRT_MAX_ADDRS	32
#define NETVIRT_MAX_ROUTES	256
#define NETVIRT_MTU_DEFAULT	1500

/* Defaults for the two mapped devices and the loopback.  */
#define NETVIRT_VETH_NAME	"veth0"
#define NETVIRT_VTUN_NAME	"vtun"
#define NETVIRT_VETH_ADDR	"10.177.0.2"
#define NETVIRT_VETH_PREFIX	24
#define NETVIRT_VETH_GW		"10.177.0.1"
#define NETVIRT_VTUN_ADDR	"100.64.0.2"
#define NETVIRT_VTUN_PREFIX	32
#define NETVIRT_VTUN_ROUTE	"100.64.0.0"

/* Routing scopes, tables and route types.  They are duplicated here
 * rather than pulled from <linux/rtnetlink.h> so that the file builds
 * against the same headers on glibc and bionic.  */
#define NETVIRT_SCOPE_UNIVERSE	0
#define NETVIRT_SCOPE_SITE	200
#define NETVIRT_SCOPE_LINK	253
#define NETVIRT_SCOPE_HOST	254
#define NETVIRT_SCOPE_NOWHERE	255

#define NETVIRT_TABLE_UNSPEC	0
#define NETVIRT_TABLE_MAIN	254
#define NETVIRT_TABLE_LOCAL	255

#define NETVIRT_PROTO_UNSPEC	0
#define NETVIRT_PROTO_KERNEL	2
#define NETVIRT_PROTO_BOOT	3
#define NETVIRT_PROTO_STATIC	4

#define NETVIRT_RTN_UNSPEC	0
#define NETVIRT_RTN_UNICAST	1
#define NETVIRT_RTN_LOCAL	2
#define NETVIRT_RTN_BROADCAST	3
#define NETVIRT_RTN_ANYCAST	5

/* ARPHRD_* values from <linux/if_arp.h>.  */
#define NETVIRT_ARPHRD_ETHER	1
#define NETVIRT_ARPHRD_LOOPBACK	772
#define NETVIRT_ARPHRD_NONE	0xFFFE

/* IF_OPER_* values from <linux/if.h>.  */
#define NETVIRT_OPER_UNKNOWN		0
#define NETVIRT_OPER_NOTPRESENT		1
#define NETVIRT_OPER_DOWN		2
#define NETVIRT_OPER_LOWERLAYERDOWN	3
#define NETVIRT_OPER_TESTING		4
#define NETVIRT_OPER_DORMANT		5
#define NETVIRT_OPER_UP			6

typedef enum {
    NETVIRT_BRIDGE_NONE = 0,
    /* User-mode NAT: ordinary I/O goes through the host stack.  */
    NETVIRT_BRIDGE_NAT,
    /* An external user-space WireGuard implementation (wireguard-go,
     * boringtun, tsnet, ...) reached over its UAPI socket.  */
    NETVIRT_BRIDGE_WG_USER,
    /* The in-kernel WireGuard driver, only usable with CAP_NET_ADMIN
     * in the current network namespace.  */
    NETVIRT_BRIDGE_WG_KERNEL,
} NetVirtBridgeKind;

/* One address assigned to a virtual device.  */
typedef struct NetVirtAddr {
    int family;			/* AF_INET or AF_INET6 */
    uint8_t prefix;		/* prefix length */
    uint8_t scope;		/* RT_SCOPE_* */
    uint8_t addr[16];
    uint8_t broadcast[16];
    bool has_broadcast;
    char label[NETVIRT_NAME_MAX];
    bool connected;		/* route auto-created for this address */
} NetVirtAddr;

typedef struct NetVirtDev {
    char name[NETVIRT_NAME_MAX];
    int index;
    unsigned flags;		/* IFF_* */
    int mtu;
    unsigned txqlen;
    int operstate;		/* IF_OPER_* */
    int arphrd;			/* ARPHRD_* */
    unsigned group;
    uint8_t mac[6];
    bool has_mac;
    bool carrier;
    char kind[16];		/* "loopback", "veth", "tun", "dummy", ... */

    /* Bridge this device is attached to.  */
    NetVirtBridgeKind bridge;
    char *wg_conf;		/* raw "key=value" WireGuard config, or NULL */
    char *bridge_status;	/* human readable, for --verbose */
    void *bridge_handle;	/* user-space library handle, when used */
    void (*bridge_destroy)(void *handle);

    NetVirtAddr addrs[NETVIRT_MAX_ADDRS];
    int naddrs;
} NetVirtDev;

typedef struct NetVirtRoute {
    int family;
    uint8_t dst[16];
    uint8_t prefix;
    uint8_t src[16];
    uint8_t src_prefix;
    bool has_gw;
    uint8_t gw[16];
    int oif;			/* device index, 0 when unset */
    char oif_name[NETVIRT_NAME_MAX];
    int table;			/* RT_TABLE_* */
    int protocol;		/* RTPROT_* */
    int scope;			/* RT_SCOPE_* */
    int type;			/* RTN_* */
    uint32_t metric;
    uint32_t flags;
    bool connected;		/* kept in sync with an address */
} NetVirtRoute;

/* One queued netlink datagram (the kernel sends one message per
 * datagram for dumps, and so do we).  */
typedef struct NetVirtFrame {
    struct NetVirtFrame *next;
    unsigned char *data;
    size_t length;
    size_t offset;		/* MSG_PEEK does not advance it */
} NetVirtFrame;

/* A netlink socket opened by the container, tracked per tracee.  */
typedef struct NetVirtFd {
    struct NetVirtFd *next;
    int fd;
    bool rtnl;			/* AF_NETLINK / NETLINK_ROUTE */
    bool tun;			/* virtual /dev/net/tun descriptor */
    char tun_name[NETVIRT_NAME_MAX];
    uint32_t local_pid;		/* nl_pid this socket is bound to */
    NetVirtFrame *frames;	/* queue head */
    NetVirtFrame *tail;
} NetVirtFd;

typedef struct NetVirtTracee {
    struct NetVirtTracee *next;
    pid_t pid;
    NetVirtFd *fds;

    /* Result of an emulated syscall, filled at the enter stage and
     * written to SYSARG_RESULT at the exit stage.  */
    bool pend_valid;
    long pend_result;
    /* The current syscall is an open("/dev/net/tun") replaced with a
     * dup() of the engine's virtual TUN descriptor.  */
    bool tun_open_pending;
} NetVirtTracee;

typedef struct NetVirtConfig {
    bool enabled;
    bool lo_disabled;

    NetVirtDev devs[NETVIRT_MAX_DEVS];
    int ndevs;

    NetVirtRoute routes[NETVIRT_MAX_ROUTES];
    int nroutes;

    int next_index;

    NetVirtBridgeKind default_bridge;
    bool bridge_explicit;
    char *wg_exec;		/* override for the user-space implementation */
    char *wg_lib;		/* override for its shared library */
    void *bridge_library;	/* dlopen() handle, resolved once */
    const void *bridge_ops;	/* struct uvroot_wg_ops *, or NULL */
    bool nat_fallback;

    /* Per-tracee state (the config itself is shared by the whole
     * container, see INHERIT_PARENT).  */
    NetVirtTracee *tracees;

    /* User-space WireGuard processes started for this container.  */
    pid_t *bridge_children;
    int bridge_nchildren;

    /* Built-in user-space WireGuard engine (see wg/).  */
    void *wg_engine;		/* WgEngine *, owned by the config */
    bool wg_engine_started;
    bool wg_start_pending;	/* engine built, thread not started yet */
    bool abi_rejected;		/* foreign-ABI error already reported */
    int tun_guest_fd;		/* end inherited by the container */
    int tun_engine_fd;		/* end read and written by the engine */
    char wg_device[NETVIRT_NAME_MAX];
} NetVirtConfig;

/* netvirt.c */
extern int netvirt_callback(Extension *extension, ExtensionEvent event,
			    intptr_t d1, intptr_t d2);

/* Command-line entry points.  Each one enables the extension.  */
extern int netvirt_enable(Tracee *tracee);
extern int netvirt_add_if(Tracee *tracee, const char *spec);
extern int netvirt_add_route(Tracee *tracee, const char *spec);
extern int netvirt_set_wg(Tracee *tracee, const char *spec);
extern int netvirt_set_bridge(Tracee *tracee, const char *spec);

/* Called once all the options are known, before the bindings are
 * canonicalized: materializes the /proc/net views and starts the
 * bridges.  */
extern int netvirt_finalize(Tracee *tracee);

/* Model helpers shared with netlink.c and ioctl.c.  */
extern NetVirtDev *netvirt_dev_by_name(NetVirtConfig *config, const char *name);
extern NetVirtDev *netvirt_dev_by_index(NetVirtConfig *config, int index);
extern NetVirtDev *netvirt_dev_alloc(NetVirtConfig *config, const char *name,
				     const char *kind);
extern void netvirt_dev_remove(NetVirtConfig *config, NetVirtDev *dev);
extern int netvirt_addr_add(NetVirtDev *dev, const NetVirtAddr *addr);
extern int netvirt_addr_del(NetVirtDev *dev, int family,
			    const uint8_t addr[16], uint8_t prefix);
extern int netvirt_route_add(NetVirtConfig *config, const NetVirtRoute *route);
extern int netvirt_route_del(NetVirtConfig *config, const NetVirtRoute *route);
extern void netvirt_route_reindex(NetVirtConfig *config);
/* Add (@add true) or remove the connected route implied by @addr.  */
extern int netvirt_addr_route(NetVirtConfig *config, const NetVirtDev *dev,
			      const NetVirtAddr *addr, bool add);
extern bool netvirt_addr_equal(const NetVirtAddr *a, int family,
			       const uint8_t addr[16], uint8_t prefix);

/* netlink.c */
extern int netvirt_netlink_request(Tracee *tracee, NetVirtConfig *config,
				   NetVirtFd *fd, const unsigned char *request,
				   size_t length);

/* Append a synthetic reply to @fd's queue (implemented in netvirt.c).  */
extern void netvirt_fd_append(NetVirtFd *fd, const void *data, size_t length);

/* ioctl.c: 1 when handled (result set), 0 when it must be passed to the
 * kernel, -errno on a hard error.  */
extern int netvirt_ioctl_request(Tracee *tracee, NetVirtConfig *config,
				 unsigned long request, word_t arg, long *result);

/* ioctl.c: the TUN* ioctls of the virtual /dev/net/tun descriptor.  */
extern int netvirt_tun_ioctl(Tracee *tracee, NetVirtConfig *config,
			     NetVirtFd *entry, unsigned long request,
			     word_t argument, long *result);

/* bridge.c */
extern void netvirt_bridge_init(Tracee *tracee, NetVirtConfig *config);
extern void netvirt_bridge_fini(NetVirtConfig *config);
extern int netvirt_bridge_attach(Tracee *tracee, NetVirtConfig *config,
				 NetVirtDev *dev);
/*
 * Start the built-in user-space WireGuard engine (the wg/ module) for
 * @dev: it creates a socketpair whose container end backs the virtual
 * /dev/net/tun, and whose other end feeds the tunnel.  Must be called
 * before the initial tracee is forked, so that the descriptor is
 * inherited.
 */
extern int netvirt_bridge_start_engine(Tracee *tracee, NetVirtConfig *config,
				       NetVirtDev *dev);
/* Start the engine thread of an already-built engine; called once the
 * tracee has been forked.  */
extern int netvirt_bridge_start_pending(Tracee *tracee,
					NetVirtConfig *config);

#endif				/* NETVIRT_H */
