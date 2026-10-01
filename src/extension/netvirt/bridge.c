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
 * The WireGuard bridge.  uvroot never needs CAP_NET_ADMIN: it talks to a
 * *user-space* WireGuard implementation, exactly like the netfs backends
 * talk to their libraries.
 *
 * Three implementations are tried, in this order:
 *
 *   1. a shared library named by UVROOT_NETVIRT_WG_LIB (or --net-bridge
 *      lib=...) exporting the small ABI below.  This is the preferred
 *      path: no extra process, no filesystem dependency;
 *
 *          struct uvroot_wg_ops {
 *              size_t abi_size;
 *              void *(*create)(const char *ifname, int tun_fd,
 *                              char *error, size_t error_size);
 *              int   (*configure)(void *handle, const char *uapi);
 *              void  (*destroy)(void *handle);
 *          };
 *
 *   2. an external user-space implementation (wireguard-go, boringtun,
 *      ...) driven through its UAPI socket, with a socketpair standing
 *      in for the TUN device (WG_TUN_FD / WG_UAPI_FD);
 *
 *   3. a user-mode NAT that relays through the host stack.  This is the
 *      fallback: the tunnel is not established, but ordinary I/O keeps
 *      working and the virtual devices stay fully controllable.
 */

#include <dlfcn.h>		/* dlopen(3), dlsym(3), */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>		/* strcasecmp(3), */
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "attribute.h"
#include "cli/note.h"
#include "extension/netvirt/netvirt.h"
#include "extension/netvirt/wg/wg_engine.h"
#include "tracee/tracee.h"

#ifndef PF_UNIX
#define PF_UNIX AF_UNIX
#endif

struct uvroot_wg_ops {
    size_t abi_size;
    void *(*create)(const char *ifname, int tun_fd, char *error,
		    size_t error_size);
    int (*configure)(void *handle, const char *uapi);
    void (*destroy)(void *handle);
};

typedef struct WgPeer {
    struct WgPeer *next;
    char *public_key;
    char *preshared_key;
    char *endpoint;
    char **allowed_ips;
    int allowed_count;
    int keepalive;
} WgPeer;

typedef struct WgConfig {
    char *private_key;
    char *address;
    int listen_port;
    WgPeer *peers;
} WgConfig;

/* ------------------------------------------------------------------ */
/* WireGuard configuration parsing                                     */
/* ------------------------------------------------------------------ */

static void wg_free(WgConfig *config)
{
    WgPeer *peer;

    if (config == NULL)
	return;
    free(config->private_key);
    free(config->address);
    for (peer = config->peers; peer != NULL;) {
	WgPeer *next = peer->next;
	int i;

	free(peer->public_key);
	free(peer->preshared_key);
	free(peer->endpoint);
	for (i = 0; i < peer->allowed_count; i++)
	    free(peer->allowed_ips[i]);
	free(peer->allowed_ips);
	free(peer);
	peer = next;
    }
    free(config);
}

static char *trim(char *text)
{
    char *end;

    while (*text == ' ' || *text == '\t' || *text == '\r')
	text++;
    end = text + strlen(text);
    while (end > text
	   && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'
	       || end[-1] == '\n'))
	   *--end = '\0';
    return text;
}

static void wg_peer_add(WgConfig *config, WgPeer *peer)
{
    WgPeer **link = &config->peers;

    while (*link != NULL)
	link = &(*link)->next;
    *link = peer;
}

/*
 * Accept both the wg(8)/wg-quick file format ([Interface]/[Peer] with
 * "Key = Value" lines) and a flat "key=value;key=value" string, so that
 * --wg=veth0:private_key=...;peer=... is usable on a command line.
 */
static WgConfig *wg_parse(const char *text)
{
    WgConfig *config;
    WgPeer *peer = NULL;
    char *copy;
    char *line;
    char *save = NULL;

    config = calloc(1, sizeof(*config));
    if (config == NULL)
	return NULL;

    copy = strdup(text);
    if (copy == NULL) {
	free(config);
	return NULL;
    }

    for (line = strtok_r(copy, "\n;", &save); line != NULL;
	 line = strtok_r(NULL, "\n;", &save)) {
	char *key;
	char *value;

	line = trim(line);
	if (line[0] == '\0' || line[0] == '#')
	    continue;
	if (line[0] == '[') {
	    if (strncasecmp(line, "[Peer]", 6) == 0) {
		peer = calloc(1, sizeof(*peer));
		if (peer != NULL)
		    wg_peer_add(config, peer);
	    }
	    continue;
	}

	key = line;
	value = strchr(line, '=');
	if (value == NULL) {
	    /* Also accept the UAPI spelling "key: value".  */
	    value = strchr(line, ':');
	}
	if (value == NULL)
	    continue;
	*value++ = '\0';
	key = trim(key);
	value = trim(value);

	if (strcasecmp(key, "PrivateKey") == 0
	    || strcasecmp(key, "private_key") == 0) {
	    free(config->private_key);
	    config->private_key = strdup(value);
	} else if (strcasecmp(key, "Address") == 0
		   || strcasecmp(key, "address") == 0) {
	    free(config->address);
	    config->address = strdup(value);
	} else if (strcasecmp(key, "ListenPort") == 0
		   || strcasecmp(key, "listen_port") == 0) {
	    config->listen_port = atoi(value);
	} else if (strcasecmp(key, "peer") == 0
		   || strcasecmp(key, "PublicKey") == 0
		   || strcasecmp(key, "public_key") == 0) {
	    if (peer == NULL || peer->public_key != NULL) {
		peer = calloc(1, sizeof(*peer));
		if (peer != NULL)
		    wg_peer_add(config, peer);
	    }
	    if (peer != NULL) {
		free(peer->public_key);
		peer->public_key = strdup(value);
	    }
	} else if (peer != NULL
		   && (strcasecmp(key, "PresharedKey") == 0
		       || strcasecmp(key, "preshared_key") == 0)) {
	    free(peer->preshared_key);
	    peer->preshared_key = strdup(value);
	} else if (peer != NULL
		   && (strcasecmp(key, "Endpoint") == 0
		       || strcasecmp(key, "endpoint") == 0)) {
	    free(peer->endpoint);
	    peer->endpoint = strdup(value);
	} else if (peer != NULL
		   && (strcasecmp(key, "AllowedIPs") == 0
		       || strcasecmp(key, "allowed_ip") == 0
		       || strcasecmp(key, "allowed_ips") == 0)) {
	    char *item;
	    char *item_save = NULL;
	    char *list = strdup(value);

	    if (list == NULL)
		continue;
	    for (item = strtok_r(list, ",", &item_save); item != NULL;
		 item = strtok_r(NULL, ",", &item_save)) {
		char **grown = realloc(peer->allowed_ips,
				       (peer->allowed_count + 1) *
				       sizeof(char *));
		if (grown == NULL)
		    break;
		peer->allowed_ips = grown;
		peer->allowed_ips[peer->allowed_count++] = strdup(trim(item));
	    }
	    free(list);
	} else if (peer != NULL
		   && (strcasecmp(key, "PersistentKeepalive") == 0
		       || strcasecmp(key,
				     "persistent_keepalive_interval") == 0)) {
	    peer->keepalive = atoi(value);
	}
    }

    free(copy);
    return config;
}

/* ------------------------------------------------------------------ */
/* Keys: wg(8) uses base64, the UAPI uses lowercase hex                */
/* ------------------------------------------------------------------ */

static int base64_decode(const char *text, uint8_t *output, size_t size)
{
    static const signed char table[256] = {
	['A'] = 0, ['B'] = 1, ['C'] = 2, ['D'] = 3, ['E'] = 4, ['F'] = 5,
	['G'] = 6, ['H'] = 7, ['I'] = 8, ['J'] = 9, ['K'] = 10, ['L'] = 11,
	['M'] = 12, ['N'] = 13, ['O'] = 14, ['P'] = 15, ['Q'] = 16,
	['R'] = 17, ['S'] = 18, ['T'] = 19, ['U'] = 20, ['V'] = 21,
	['W'] = 22, ['X'] = 23, ['Y'] = 24, ['Z'] = 25,
	['a'] = 26, ['b'] = 27, ['c'] = 28, ['d'] = 29, ['e'] = 30,
	['f'] = 31, ['g'] = 32, ['h'] = 33, ['i'] = 34, ['j'] = 35,
	['k'] = 36, ['l'] = 37, ['m'] = 38, ['n'] = 39, ['o'] = 40,
	['p'] = 41, ['q'] = 42, ['r'] = 43, ['s'] = 44, ['t'] = 45,
	['u'] = 46, ['v'] = 47, ['w'] = 48, ['x'] = 49, ['y'] = 50,
	['z'] = 51, ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55,
	['4'] = 56, ['5'] = 57, ['6'] = 58, ['7'] = 59, ['8'] = 60,
	['9'] = 61, ['+'] = 62, ['/'] = 63,
    };
    size_t written = 0;
    uint32_t accumulator = 0;
    int bits = 0;

    for (; *text != '\0' && *text != '='; text++) {
	signed char value = table[(unsigned char) *text];

	if (value < 0)
	    return -1;
	accumulator = (accumulator << 6) | (uint32_t) value;
	bits += 6;
	if (bits >= 8) {
	    bits -= 8;
	    if (written < size)
		output[written] = (uint8_t) (accumulator >> bits);
	    written++;
	}
    }
    return (written == size) ? 0 : -1;
}

static bool wg_key_to_hex(const char *key, char *output, size_t size)
{
    uint8_t raw[32];
    size_t i;

    if (size < 65)
	return false;

    /* Already hex?  */
    if (strlen(key) == 64) {
	for (i = 0; i < 64; i++) {
	    if (!((key[i] >= '0' && key[i] <= '9')
		  || (key[i] >= 'a' && key[i] <= 'f')
		  || (key[i] >= 'A' && key[i] <= 'F')))
		break;
	}
	if (i == 64) {
	    snprintf(output, size, "%s", key);
	    return true;
	}
    }

    if (base64_decode(key, raw, sizeof(raw)) < 0)
	return false;

    for (i = 0; i < sizeof(raw); i++)
	snprintf(output + i * 2, size - i * 2, "%02x", raw[i]);
    return true;
}

/* ------------------------------------------------------------------ */
/* Implementation discovery                                            */
/* ------------------------------------------------------------------ */

typedef struct BridgeImpl {
    const char *exec;		/* resolved executable path, or NULL */
    void *library;		/* dlopen() handle, or NULL */
    const struct uvroot_wg_ops *ops;
} BridgeImpl;

static char *which(const char *name)
{
    const char *path = getenv("PATH");
    char *copy;
    char *directory;
    char *save = NULL;

    if (path == NULL)
	return NULL;
    copy = strdup(path);
    if (copy == NULL)
	return NULL;

    for (directory = strtok_r(copy, ":", &save); directory != NULL;
	 directory = strtok_r(NULL, ":", &save)) {
	char candidate[4096];

	snprintf(candidate, sizeof(candidate), "%s/%s", directory, name);
	if (access(candidate, X_OK) == 0) {
	    char *result = strdup(candidate);
	    free(copy);
	    return result;
	}
    }
    free(copy);
    return NULL;
}

static void bridge_discover(Tracee *tracee, NetVirtConfig *config,
			    BridgeImpl *impl)
{
    const char *library;
    const char *environment;

    memset(impl, 0, sizeof(*impl));

    environment = getenv("UVROOT_NETVIRT_WG_EXEC");
    if (config->wg_exec != NULL)
	impl->exec = config->wg_exec;
    else if (environment != NULL)
	impl->exec = environment;
    else {
	const char *candidates[] = {
	    "wireguard-go", "boringtun-cli", "boringtun", NULL
	};
	int i;

	for (i = 0; candidates[i] != NULL; i++) {
	    char *resolved = which(candidates[i]);

	    if (resolved != NULL) {
		impl->exec = talloc_strdup(config, resolved);
		free(resolved);
		break;
	    }
	}
    }

    library = config->wg_lib != NULL ? config->wg_lib
	: getenv("UVROOT_NETVIRT_WG_LIB");
    if (library != NULL) {
	void *handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);

	if (handle == NULL) {
	    note(tracee, WARNING, USER,
		 "netvirt: cannot load %s: %s", library, dlerror());
	} else {
	    const struct uvroot_wg_ops *ops =
		(const struct uvroot_wg_ops *) dlsym(handle, "uvroot_wg_ops");

	    if (ops == NULL || ops->create == NULL || ops->configure == NULL)
		note(tracee, WARNING, USER,
		     "netvirt: %s does not export uvroot_wg_ops, ignored",
		     library);
	    else {
		impl->library = handle;
		impl->ops = ops;
		VERBOSE(tracee, 1,
			"netvirt: user-space WireGuard library %s loaded",
			library);
	    }
	}
    }

    if (impl->exec != NULL)
	VERBOSE(tracee, 1, "netvirt: user-space WireGuard %s", impl->exec);
}

/* ------------------------------------------------------------------ */
/* UAPI configuration                                                  */
/* ------------------------------------------------------------------ */

static int uapi_write_all(int fd, const char *text, size_t length)
{
    size_t done = 0;

    while (done < length) {
	ssize_t written = write(fd, text + done, length - done);

	if (written < 0) {
	    if (errno == EINTR)
		continue;
	    return -1;
	}
	done += (size_t) written;
    }
    return 0;
}

static char *uapi_build(const WgConfig *config)
{
    char *text;
    size_t size = 8192;
    size_t used = 0;
    WgPeer *peer;

    text = malloc(size);
    if (text == NULL)
	return NULL;
    text[0] = '\0';

#define APPEND(...)							\
    do {								\
	int needed = snprintf(text + used, size - used, __VA_ARGS__);	\
	if (needed < 0) {						\
	    free(text);							\
	    return NULL;						\
	}								\
	if ((size_t) needed >= size - used) {				\
	    char *grown = realloc(text, size + 8192);			\
	    if (grown == NULL) {					\
		free(text);						\
		return NULL;						\
	    }								\
	    text = grown;						\
	    size += 8192;						\
	    needed = snprintf(text + used, size - used, __VA_ARGS__);	\
	    if (needed < 0) {						\
		free(text);						\
		return NULL;						\
	    }								\
	}								\
	used += (size_t) needed;					\
    } while (0)

    APPEND("set=1\n");
    if (config->private_key != NULL) {
	char hex[80];

	if (wg_key_to_hex(config->private_key, hex, sizeof(hex)))
	    APPEND("private_key=%s\n", hex);
    }
    if (config->listen_port > 0)
	APPEND("listen_port=%d\n", config->listen_port);
    APPEND("replace_peers=true\n");

    for (peer = config->peers; peer != NULL; peer = peer->next) {
	char hex[80];
	int i;

	if (peer->public_key == NULL)
	    continue;
	if (!wg_key_to_hex(peer->public_key, hex, sizeof(hex)))
	    continue;
	APPEND("public_key=%s\n", hex);

	if (peer->preshared_key != NULL
	    && wg_key_to_hex(peer->preshared_key, hex, sizeof(hex)))
	    APPEND("preshared_key=%s\n", hex);
	if (peer->endpoint != NULL)
	    APPEND("endpoint=%s\n", peer->endpoint);
	if (peer->keepalive > 0)
	    APPEND("persistent_keepalive_interval=%d\n", peer->keepalive);

	for (i = 0; i < peer->allowed_count; i++) {
	    if (peer->allowed_ips[i] != NULL)
		APPEND("allowed_ip=%s\n", peer->allowed_ips[i]);
	}
    }
    APPEND("\n");
#undef APPEND

    return text;
}

static int uapi_configure(int fd, const WgConfig *config)
{
    char *text = uapi_build(config);
    size_t length;

    if (text == NULL)
	return -1;
    length = strlen(text);
    if (uapi_write_all(fd, text, length) < 0) {
	free(text);
	return -1;
    }
    free(text);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Attach                                                              */
/* ------------------------------------------------------------------ */

static void bridge_remember(NetVirtConfig *config, pid_t pid)
{
    pid_t *grown = talloc_realloc(config, config->bridge_children, pid_t,
				  config->bridge_nchildren + 1);

    if (grown == NULL)
	return;
    config->bridge_children = grown;
    config->bridge_children[config->bridge_nchildren++] = pid;
}

static int attach_external(Tracee *tracee, NetVirtConfig *config,
			   NetVirtDev *dev, WgConfig *wg)
{
    int tun[2];
    int uapi[2];
    pid_t pid;

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, tun) < 0)
	return -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, uapi) < 0) {
	close(tun[0]);
	close(tun[1]);
	return -1;
    }

    pid = fork();
    if (pid < 0) {
	close(tun[0]);
	close(tun[1]);
	close(uapi[0]);
	close(uapi[1]);
	return -1;
    }
    if (pid == 0) {
	char tun_fd[32];
	char uapi_fd[32];

	close(tun[0]);
	close(uapi[0]);

	snprintf(tun_fd, sizeof(tun_fd), "%d", tun[1]);
	snprintf(uapi_fd, sizeof(uapi_fd), "%d", uapi[1]);
	setenv("WG_TUN_FD", tun_fd, 1);
	setenv("WG_UAPI_FD", uapi_fd, 1);

	execl(config->wg_exec, config->wg_exec, dev->name, (char *) NULL);
	_exit(127);
    }

    close(tun[1]);
    close(uapi[1]);
    bridge_remember(config, pid);

    if (uapi_configure(uapi[0], wg) < 0) {
	note(tracee, WARNING, USER,
	     "netvirt: cannot configure %s through the WireGuard UAPI",
	     config->wg_exec);
	close(tun[0]);
	close(uapi[0]);
	return -1;
    }

    /* The TUN end stays open in this process: it is the device's data
     * path.  The UAPI socket is only needed for configuration.  */
    close(uapi[0]);
    dev->bridge_status = talloc_asprintf(config,
					 "wireguard via %s (pid %d)",
					 config->wg_exec, (int) pid);
    return 0;
}

static int attach_library(Tracee *tracee, NetVirtConfig *config,
			  NetVirtDev *dev, WgConfig *wg, BridgeImpl *impl)
{
    int tun[2];
    char error[256];
    char *uapi;
    void *handle;

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, tun) < 0)
	return -1;

    error[0] = '\0';
    handle = impl->ops->create(dev->name, tun[1], error, sizeof(error));
    if (handle == NULL) {
	note(tracee, WARNING, USER, "netvirt: %s: %s", dev->name,
	     error[0] != '\0' ? error : "cannot create the tunnel");
	close(tun[0]);
	close(tun[1]);
	return -1;
    }

    uapi = uapi_build(wg);
    if (uapi == NULL || impl->ops->configure(handle, uapi) < 0) {
	note(tracee, WARNING, USER,
	     "netvirt: cannot configure %s through the library", dev->name);
	free(uapi);
	if (impl->ops->destroy != NULL)
	    impl->ops->destroy(handle);
	close(tun[0]);
	close(tun[1]);
	return -1;
    }
    free(uapi);

    close(tun[1]);
    dev->bridge_handle = handle;
    dev->bridge_destroy = impl->ops->destroy;
    dev->bridge_status = talloc_asprintf(config,
					 "wireguard via user-space library");
    return 0;
}

int netvirt_bridge_attach(Tracee *tracee, NetVirtConfig *config,
			  NetVirtDev *dev)
{
    BridgeImpl impl;
    WgConfig *wg;
    const char *text = dev->wg_conf;

    if (text == NULL)
	return 0;

    /* conf=/path reads a wg-quick style file.  */
    if (strncmp(text, "conf=", 5) == 0) {
	FILE *file = fopen(text + 5, "r");
	char buffer[8192];
	size_t length;

	if (file == NULL) {
	    note(tracee, ERROR, USER, "netvirt: cannot read %s", text + 5);
	    return -1;
	}
	length = fread(buffer, 1, sizeof(buffer) - 1, file);
	fclose(file);
	buffer[length] = '\0';
	text = talloc_strdup(config, buffer);
	if (text == NULL)
	    return -1;
    }

    wg = wg_parse(text);
    if (wg == NULL)
	return -1;

    /* The implementation was probed once, in netvirt_bridge_init().  */
    impl.exec = config->wg_exec;
    impl.library = config->bridge_library;
    impl.ops = config->bridge_ops;

    if (impl.ops != NULL) {
	if (attach_library(tracee, config, dev, wg, &impl) == 0) {
	    wg_free(wg);
	    return 0;
	}
    }

    if (impl.exec != NULL) {
	config->wg_exec = talloc_strdup(config, impl.exec);
	if (attach_external(tracee, config, dev, wg) == 0) {
	    wg_free(wg);
	    return 0;
	}
    }

    wg_free(wg);

    /* Nothing usable: keep the device and relay through the host.  */
    dev->bridge = NETVIRT_BRIDGE_NAT;
    dev->bridge_status = talloc_asprintf(config, "nat (no user-space "
					 "WireGuard implementation)");
    return -1;
}

static void engine_log_callback(void *opaque, int level, const char *message)
{
    Tracee *tracee = (Tracee *) opaque;

    VERBOSE(tracee, level <= 1 ? 1 : level, "%s", message);
}

int netvirt_bridge_start_engine(Tracee *tracee, NetVirtConfig *config,
				NetVirtDev *dev)
{
    WgEngineConfig wgconfig;
    WgEngine *engine;
    char error[256];
    int sockets[2];

    if (config->wg_engine_started || dev->wg_conf == NULL)
	return -1;

    if (wg_engine_config_parse(&wgconfig, dev->wg_conf, error,
			       sizeof(error)) < 0) {
	note(tracee, WARNING, USER, "netvirt: %s: %s", dev->name, error);
	return -1;
    }
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) < 0) {
	wg_engine_config_free(&wgconfig);
	note(tracee, WARNING, USER, "netvirt: socketpair: %s",
	     strerror(errno));
	return -1;
    }

    /* The engine reads and writes sockets[1]; the container gets a dup
     * of sockets[0] when it opens /dev/net/tun.  */
    engine = wg_engine_new(&wgconfig, sockets[1], NULL, NULL, error,
			   sizeof(error));
    wg_engine_config_free(&wgconfig);
    if (engine == NULL) {
	note(tracee, WARNING, USER, "netvirt: WireGuard engine: %s", error);
	close(sockets[0]);
	close(sockets[1]);
	return -1;
    }
    wg_engine_set_log(engine, engine_log_callback, tracee);

    config->wg_engine = engine;
    config->wg_start_pending = true;
    config->tun_guest_fd = sockets[0];
    config->tun_engine_fd = sockets[1];
    config->wg_engine_started = true;
    /* The engine end must not leak into the container across exec; the
     * container end must, so that dup() can hand it out later.  */
    (void) fcntl(sockets[1], F_SETFD, FD_CLOEXEC);
    snprintf(config->wg_device, sizeof(config->wg_device), "%s", dev->name);

    dev->bridge = NETVIRT_BRIDGE_WG_USER;
    dev->bridge_status = talloc_asprintf(config,
					 "wireguard (built-in user-space "
					 "engine, %s)", dev->name);
    VERBOSE(tracee, 1,
	    "netvirt: built-in WireGuard engine on %s (tun fd %d)",
	    dev->name, sockets[0]);
    return 0;
}

int netvirt_bridge_start_pending(Tracee *tracee, NetVirtConfig *config)
{
    if (config->wg_engine == NULL)
	return -1;
    if (wg_engine_start(config->wg_engine) < 0) {
	note(tracee, WARNING, USER,
	     "netvirt: cannot start the WireGuard engine");
	return -1;
    }
    VERBOSE(tracee, 1, "netvirt: WireGuard engine started");
    return 0;
}

void netvirt_bridge_init(Tracee *tracee, NetVirtConfig *config)
{
    BridgeImpl impl;

    if (config == NULL)
	return;

    bridge_discover(tracee, config, &impl);

    if (impl.exec != NULL && config->wg_exec == NULL)
	config->wg_exec = talloc_strdup(config, impl.exec);
    config->bridge_library = impl.library;
    config->bridge_ops = impl.ops;

    if (impl.exec == NULL && impl.ops == NULL)
	VERBOSE(tracee, 1,
		"netvirt: no user-space WireGuard implementation found, "
		"devices fall back to the user-mode NAT bridge");
}

void netvirt_bridge_fini(NetVirtConfig *config)
{
    int i;

    if (config == NULL)
	return;

    if (config->wg_engine != NULL) {
	wg_engine_stop(config->wg_engine);
	wg_engine_free(config->wg_engine);
	config->wg_engine = NULL;
    }
    if (config->tun_guest_fd >= 0) {
	close(config->tun_guest_fd);
	config->tun_guest_fd = -1;
    }
    if (config->tun_engine_fd >= 0) {
	close(config->tun_engine_fd);
	config->tun_engine_fd = -1;
    }
    config->wg_engine_started = false;

    /* Release the tunnels created through a user-space library.  */
    for (i = 0; i < config->ndevs; i++) {
	NetVirtDev *dev = &config->devs[i];

	if (dev->bridge_handle == NULL)
	    continue;
	if (dev->bridge_destroy != NULL)
	    dev->bridge_destroy(dev->bridge_handle);
	dev->bridge_handle = NULL;
	dev->bridge_destroy = NULL;
    }

    for (i = 0; i < config->bridge_nchildren; i++) {
	pid_t pid = config->bridge_children[i];

	if (pid > 0)
	    (void) kill(pid, SIGTERM);
    }
    for (i = 0; i < config->bridge_nchildren; i++) {
	if (config->bridge_children[i] > 0)
	    (void) waitpid(config->bridge_children[i], NULL, 0);
    }
    config->bridge_nchildren = 0;
}
