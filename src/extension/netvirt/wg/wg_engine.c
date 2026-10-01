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

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "wg_crypto.h"
#include "wg_engine.h"
#include "wg_tcp.h"
#include "wg_udp.h"
#include "wg_noise.h"

#define WG_REKEY_TIMEOUT_MS	5000
#define WG_REKEY_ATTEMPT_MS	90000
#define WG_KEEPALIVE_MS		10000
#define WG_REKEY_AFTER_MS	120000

typedef struct WgQueued {
    uint8_t data[WG_MAX_PACKET];
    size_t length;
} WgQueued;

typedef struct WgPeer {
    WgPeerConfig *config;
    struct sockaddr_storage endpoint;
    socklen_t endpoint_length;

    WgNoise noise;
    bool have_session;
    bool handshake_pending;
    uint8_t send_key[32];
    uint8_t recv_key[32];
    uint32_t local_index;
    uint32_t remote_index;
    uint64_t send_counter;

    uint64_t recv_last;
    uint64_t recv_bitmap;
    bool recv_initialized;

    uint64_t last_initiation_ms;
    uint64_t handshake_started_ms;
    uint64_t session_born_ms;
    uint64_t last_data_sent_ms;
    uint64_t last_data_received_ms;
    bool keepalive_pending;

    WgQueued queue[WG_QUEUED_PACKETS];
    int queue_count;
} WgPeer;

struct WgEngine {
    WgEngineConfig config;
    int udp_fd;
    int tun_fd;
    WgTcp *tcp;
    WgUdp *udp_forward;
    WgPacketCallback callback;
    void *opaque;
    WgLogCallback log;
    void *log_opaque;

    WgPeer peers[WG_MAX_PEERS];
    int peer_count;

    pthread_t thread;
    volatile bool running;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint64_t monotonic_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * 1000 + (uint64_t) now.tv_nsec / 1000000;
}

static bool is_zero_bytes(const uint8_t *data, size_t length)
{
    size_t i;

    for (i = 0; i < length; i++) {
	if (data[i] != 0)
	    return false;
    }
    return true;
}

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t) value;
    p[1] = (uint8_t) (value >> 8);
    p[2] = (uint8_t) (value >> 16);
    p[3] = (uint8_t) (value >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8)
	| ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void put_le64(uint8_t *p, uint64_t value)
{
    int i;

    for (i = 0; i < 8; i++)
	p[i] = (uint8_t) (value >> (8 * i));
}

static uint64_t get_le64(const uint8_t *p)
{
    uint64_t value = 0;
    int i;

    for (i = 0; i < 8; i++)
	value |= ((uint64_t) p[i]) << (8 * i);
    return value;
}

static void engine_log(WgEngine *engine, int level, const char *format, ...)
{
    char message[512];
    va_list arguments;

    if (engine->log == NULL)
	return;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    engine->log(engine->log_opaque, level, message);
}

/* ------------------------------------------------------------------ */
/* Key decoding                                                        */
/* ------------------------------------------------------------------ */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
	return c - '0';
    if (c >= 'a' && c <= 'f')
	return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
	return c - 'A' + 10;
    return -1;
}

static int decode_key(const char *text, uint8_t output[32])
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
    size_t length = strlen(text);
    size_t i;

    if (length == 64) {
	for (i = 0; i < 64; i++) {
	    int high = hex_value(text[i]);
	    int low = hex_value(text[i + 1]);

	    if (high < 0 || low < 0)
		break;
	    output[i / 2] = (uint8_t) ((high << 4) | low);
	    i++;
	}
	if (i == 64)
	    return 0;
    }

    {
	uint32_t accumulator = 0;
	int bits = 0;
	size_t written = 0;

	for (i = 0; i < length && text[i] != '='; i++) {
	    signed char value = table[(unsigned char) text[i]];

	    if (value < 0)
		return -1;
	    accumulator = (accumulator << 6) | (uint32_t) value;
	    bits += 6;
	    if (bits >= 8) {
		bits -= 8;
		if (written < 32)
		    output[written] = (uint8_t) (accumulator >> bits);
		written++;
	    }
	}
	if (written != 32)
	    return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

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

static int parse_allowed(WgPeerConfig *peer, const char *text)
{
    char *copy = strdup(text);
    char *item;
    char *save = NULL;

    if (copy == NULL)
	return -1;
    for (item = strtok_r(copy, ",", &save); item != NULL;
	 item = strtok_r(NULL, ",", &save)) {
	char *slash;
	int prefix = -1;
	WgAllowedIp *allowed;

	if (peer->allowed_count >= WG_MAX_ALLOWED)
	    break;
	item = trim(item);
	if (item[0] == '\0')
	    continue;
	slash = strchr(item, '/');
	if (slash != NULL) {
	    *slash = '\0';
	    prefix = atoi(slash + 1);
	}

	allowed = &peer->allowed[peer->allowed_count];
	if (inet_pton(AF_INET, item, allowed->address) == 1) {
	    allowed->family = AF_INET;
	    if (prefix < 0 || prefix > 32)
		prefix = 32;
	} else if (inet_pton(AF_INET6, item, allowed->address) == 1) {
	    allowed->family = AF_INET6;
	    if (prefix < 0 || prefix > 128)
		prefix = 128;
	} else
	    continue;
	allowed->prefix = (uint8_t) prefix;
	peer->allowed_count++;
    }
    free(copy);
    return 0;
}

/*
 * "forward=LOCALIP:PORT=HOST:PORT": expose a local TCP service through
 * the tunnel.
 */
static int parse_forward(WgEngineConfig *config, const char *value, bool udp)
{
    char copy[256];
    char *equal;
    char *local;
    char *host;
    char *colon;
    struct in_addr address;
    WgForwardConfig *forward;

    if ((udp ? config->udp_forward_count : config->forward_count)
	>= WG_MAX_FORWARDS)
	return -1;
    snprintf(copy, sizeof(copy), "%s", value);
    equal = strchr(copy, '=');
    if (equal == NULL)
	return -1;
    *equal = '\0';
    local = copy;
    host = equal + 1;

    colon = strrchr(local, ':');
    if (colon == NULL)
	return -1;
    *colon = '\0';
    if (inet_pton(AF_INET, local, &address) != 1)
	return -1;
    forward = udp ? &config->udp_forwards[config->udp_forward_count]
	: &config->forwards[config->forward_count];
    forward->local_ip = ntohl(address.s_addr);
    forward->local_port = (uint16_t) atoi(colon + 1);

    colon = strrchr(host, ':');
    if (colon == NULL)
	return -1;
    *colon = '\0';
    if (inet_pton(AF_INET, host, &address) != 1)
	return -1;
    forward->host_ip = ntohl(address.s_addr);
    forward->host_port = (uint16_t) atoi(colon + 1);

    if (forward->local_port == 0 || forward->host_port == 0)
	return -1;
    if (udp)
	config->udp_forward_count++;
    else
	config->forward_count++;
    return 0;
}

int wg_engine_config_parse(WgEngineConfig *config, const char *text,
			   char *error, size_t error_size)
{
    char *copy;
    char *line;
    char *save = NULL;
    WgPeerConfig *peer = NULL;

    memset(config, 0, sizeof(*config));
    config->listen_port = 0;
    config->forward_count = 0;
    config->udp_forward_count = 0;

    copy = strdup(text);
    if (copy == NULL)
	return -1;

    for (line = strtok_r(copy, "\n;", &save); line != NULL;
	 line = strtok_r(NULL, "\n;", &save)) {
	char *key;
	char *value;

	line = trim(line);
	if (line[0] == '\0' || line[0] == '#')
	    continue;
	if (line[0] == '[') {
	    if (strncasecmp(line, "[Peer]", 6) == 0)
		peer = NULL;
	    continue;
	}

	key = line;
	value = strchr(line, '=');
	if (value == NULL)
	    value = strchr(line, ':');
	if (value == NULL)
	    continue;
	*value++ = '\0';
	key = trim(key);
	value = trim(value);

	if (strcasecmp(key, "PrivateKey") == 0
	    || strcasecmp(key, "private_key") == 0) {
	    if (decode_key(value, config->private_key) < 0) {
		snprintf(error, error_size, "invalid PrivateKey");
		free(copy);
		return -1;
	    }
	} else if (strcasecmp(key, "ListenPort") == 0
		   || strcasecmp(key, "listen_port") == 0) {
	    config->listen_port = (uint16_t) atoi(value);
	} else if (strcasecmp(key, "Forward") == 0
		   || strcasecmp(key, "forward") == 0) {
	    if (parse_forward(config, value, false) < 0) {
		snprintf(error, error_size, "invalid forward \"%s\"", value);
		free(copy);
		return -1;
	    }
	} else if (strcasecmp(key, "ForwardUdp") == 0
		   || strcasecmp(key, "forward_udp") == 0) {
	    if (parse_forward(config, value, true) < 0) {
		snprintf(error, error_size,
			 "invalid udp forward \"%s\"", value);
		free(copy);
		return -1;
	    }
	} else if (strcasecmp(key, "Address") == 0
		   || strcasecmp(key, "address") == 0) {
	    /* Handled by the network model, not by the tunnel.  */
	    continue;
	} else if (strcasecmp(key, "PublicKey") == 0
		   || strcasecmp(key, "public_key") == 0
		   || strcasecmp(key, "peer") == 0
		   || strncasecmp(key, "peer_public", 11) == 0) {
	    WgPeerConfig *fresh = calloc(1, sizeof(*fresh));
	    WgPeerConfig **link;

	    if (fresh == NULL) {
		free(copy);
		return -1;
	    }
	    if (decode_key(value, fresh->public_key) < 0) {
		snprintf(error, error_size, "invalid PublicKey");
		free(fresh);
		free(copy);
		return -1;
	    }
	    link = &config->peers;
	    while (*link != NULL)
		link = &(*link)->next;
	    *link = fresh;
	    peer = fresh;
	} else if (peer == NULL) {
	    continue;
	} else if (strcasecmp(key, "PresharedKey") == 0
		   || strcasecmp(key, "preshared_key") == 0) {
	    if (decode_key(value, peer->preshared_key) == 0)
		peer->has_preshared_key = true;
	} else if (strcasecmp(key, "Endpoint") == 0
		   || strcasecmp(key, "endpoint") == 0) {
	    snprintf(peer->endpoint, sizeof(peer->endpoint), "%s", value);
	} else if (strcasecmp(key, "AllowedIPs") == 0
		   || strcasecmp(key, "allowed_ip") == 0
		   || strcasecmp(key, "allowed_ips") == 0) {
	    parse_allowed(peer, value);
	} else if (strcasecmp(key, "PersistentKeepalive") == 0
		   || strcasecmp(key,
				 "persistent_keepalive_interval") == 0) {
	    peer->keepalive = atoi(value);
	}
    }

    free(copy);

    if (is_zero_bytes(config->private_key, 32)) {
	snprintf(error, error_size, "no PrivateKey");
	return -1;
    }
    if (config->peers == NULL) {
	snprintf(error, error_size, "no peer");
	return -1;
    }
    if (!wg_crypto_init()) {
	snprintf(error, error_size, "%s", wg_crypto_error());
	return -1;
    }
    if (wg_x25519_public(config->public_key, config->private_key) < 0) {
	snprintf(error, error_size, "cannot derive the public key");
	return -1;
    }
    return 0;
}

void wg_engine_config_free(WgEngineConfig *config)
{
    WgPeerConfig *peer = config->peers;

    while (peer != NULL) {
	WgPeerConfig *next = peer->next;

	free(peer);
	peer = next;
    }
    config->peers = NULL;
}

/* ------------------------------------------------------------------ */
/* Endpoint resolution                                                 */
/* ------------------------------------------------------------------ */

static int resolve_endpoint(const char *text, struct sockaddr_storage *storage,
			    socklen_t *length)
{
    char host[256];
    char service[32];
    const char *colon;
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    int status;

    if (text == NULL || text[0] == '\0')
	return -1;
    snprintf(host, sizeof(host), "%s", text);

    if (host[0] == '[') {
	char *closing = strchr(host, ']');

	if (closing == NULL)
	    return -1;
	*closing = '\0';
	snprintf(service, sizeof(service), "%s",
		 closing[1] == ':' ? closing + 2 : "51820");
	memmove(host, host + 1, strlen(host));
    } else {
	colon = strrchr(host, ':');
	if (colon != NULL) {
	    snprintf(service, sizeof(service), "%s", colon + 1);
	    ((char *) colon)[0] = '\0';
	} else
	    snprintf(service, sizeof(service), "51820");
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    status = getaddrinfo(host, service, &hints, &result);
    if (status != 0 || result == NULL)
	return -1;
    memcpy(storage, result->ai_addr, result->ai_addrlen);
    *length = (socklen_t) result->ai_addrlen;
    freeaddrinfo(result);
    return 0;
}

/* A dual-stack AF_INET6 socket cannot send to an AF_INET sockaddr;
 * turn the latter into its IPv4-mapped AF_INET6 form.  */
static void normalize_endpoint(struct sockaddr_storage *storage,
			       socklen_t *length, int socket_family)
{
    if (socket_family == AF_INET6 && storage->ss_family == AF_INET) {
	struct sockaddr_in *v4 = (struct sockaddr_in *) storage;
	struct sockaddr_in6 v6;

	memset(&v6, 0, sizeof(v6));
	v6.sin6_family = AF_INET6;
	v6.sin6_port = v4->sin_port;
	v6.sin6_addr.s6_addr[10] = 0xFF;
	v6.sin6_addr.s6_addr[11] = 0xFF;
	memcpy(&v6.sin6_addr.s6_addr[12], &v4->sin_addr, 4);
	memcpy(storage, &v6, sizeof(v6));
	*length = sizeof(v6);
    }
}

/* ------------------------------------------------------------------ */
/* Engine                                                              */
/* ------------------------------------------------------------------ */

static uint32_t new_index(void)
{
    uint32_t index = 0;

    while (index == 0)
	wg_random((uint8_t *) &index, sizeof(index));
    return index;
}

static int engine_tcp_send(void *opaque, const uint8_t *packet, size_t length)
{
    return wg_engine_send_ip(opaque, packet, length);
}

WgEngine *wg_engine_new(const WgEngineConfig *config, int tun_fd,
			WgPacketCallback callback, void *opaque,
			char *error, size_t error_size)
{
    WgEngine *engine;
    struct sockaddr_in6 address;
    WgPeerConfig *peer_config;
    int index = 0;

    if (!wg_crypto_init()) {
	snprintf(error, error_size, "%s", wg_crypto_error());
	return NULL;
    }

    engine = calloc(1, sizeof(*engine));
    if (engine == NULL) {
	snprintf(error, error_size, "out of memory");
	return NULL;
    }
    engine->config = *config;
    engine->config.peers = NULL;
    engine->tun_fd = tun_fd;
    engine->callback = callback;
    engine->opaque = opaque;
    engine->running = false;

    /* Deep-copy the peer list without its `next` links pointing at the
     * caller's configuration.  */
    {
	WgPeerConfig **link = &engine->config.peers;

	for (peer_config = config->peers; peer_config != NULL;
	     peer_config = peer_config->next) {
	    WgPeerConfig *fresh = calloc(1, sizeof(*fresh));

	    if (fresh == NULL) {
		snprintf(error, error_size, "out of memory");
		wg_engine_free(engine);
		return NULL;
	    }
	    *fresh = *peer_config;
	    fresh->next = NULL;
	    *link = fresh;
	    link = &fresh->next;
	}
    }

    engine->udp_fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (engine->udp_fd >= 0) {
	int off = 0;

	/* One dual-stack socket keeps the code simple.  */
	setsockopt(engine->udp_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off,
		   sizeof(off));
	memset(&address, 0, sizeof(address));
	address.sin6_family = AF_INET6;
	address.sin6_addr = in6addr_any;
	address.sin6_port = htons(config->listen_port);
	if (bind(engine->udp_fd, (struct sockaddr *) &address,
		 sizeof(address)) < 0) {
	    close(engine->udp_fd);
	    engine->udp_fd = -1;
	}
    }
    if (engine->udp_fd >= 0)
	(void) fcntl(engine->udp_fd, F_SETFD, FD_CLOEXEC);
    if (engine->udp_fd < 0) {
	struct sockaddr_in legacy;

	engine->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (engine->udp_fd >= 0)
	    (void) fcntl(engine->udp_fd, F_SETFD, FD_CLOEXEC);
	if (engine->udp_fd < 0) {
	    snprintf(error, error_size, "socket: %s", strerror(errno));
	    wg_engine_free(engine);
	    return NULL;
	}
	memset(&legacy, 0, sizeof(legacy));
	legacy.sin_family = AF_INET;
	legacy.sin_addr.s_addr = htonl(INADDR_ANY);
	legacy.sin_port = htons(config->listen_port);
	if (bind(engine->udp_fd, (struct sockaddr *) &legacy,
		 sizeof(legacy)) < 0) {
	    snprintf(error, error_size, "bind port %u: %s",
		     config->listen_port, strerror(errno));
	    wg_engine_free(engine);
	    return NULL;
	}
    }

    for (peer_config = engine->config.peers; peer_config != NULL;
	 peer_config = peer_config->next) {
	if (index >= WG_MAX_PEERS)
	    break;
	engine->peers[index].config = peer_config;
	if (peer_config->endpoint[0] != '\0') {
	    (void) resolve_endpoint(peer_config->endpoint,
				    &engine->peers[index].endpoint,
				    &engine->peers[index].endpoint_length);
	    normalize_endpoint(&engine->peers[index].endpoint,
			       &engine->peers[index].endpoint_length,
			       AF_INET6);
	}
	index++;
    }
    engine->peer_count = index;

    if (engine->peer_count == 0) {
	snprintf(error, error_size, "no peer");
	wg_engine_free(engine);
	return NULL;
    }

    if (config->forward_count > 0) {
	int forward;

	engine->tcp = wg_tcp_new(engine_tcp_send, engine);
	if (engine->tcp == NULL) {
	    snprintf(error, error_size, "out of memory");
	    wg_engine_free(engine);
	    return NULL;
	}
	for (forward = 0; forward < config->forward_count; forward++)
	    (void) wg_tcp_add_forward(engine->tcp,
				      config->forwards[forward].local_ip,
				      config->forwards[forward].local_port,
				      config->forwards[forward].host_ip,
				      config->forwards[forward].host_port);
    }

    if (config->udp_forward_count > 0) {
	int forward;

	engine->udp_forward = wg_udp_new(engine_tcp_send, engine);
	if (engine->udp_forward == NULL) {
	    snprintf(error, error_size, "out of memory");
	    wg_engine_free(engine);
	    return NULL;
	}
	for (forward = 0; forward < config->udp_forward_count; forward++)
	    (void) wg_udp_add_forward(engine->udp_forward,
				      config->udp_forwards[forward].local_ip,
				      config->udp_forwards[forward].local_port,
				      config->udp_forwards[forward].host_ip,
				      config->udp_forwards[forward].host_port);
    }

    snprintf(error, error_size, "ok");
    return engine;
}

void wg_engine_free(WgEngine *engine)
{
    if (engine->tcp != NULL) {
	wg_tcp_free(engine->tcp);
	engine->tcp = NULL;
    }
    if (engine->udp_forward != NULL) {
	wg_udp_free(engine->udp_forward);
	engine->udp_forward = NULL;
    }
    if (engine == NULL)
	return;
    wg_engine_stop(engine);
    if (engine->udp_fd >= 0)
	close(engine->udp_fd);
    wg_engine_config_free(&engine->config);
    free(engine);
}

void wg_engine_set_log(WgEngine *engine, WgLogCallback log, void *opaque)
{
    engine->log = log;
    engine->log_opaque = opaque;
}

bool wg_engine_ready(WgEngine *engine)
{
    int i;

    for (i = 0; i < engine->peer_count; i++) {
	if (engine->peers[i].have_session)
	    return true;
    }
    return false;
}

static void send_raw(WgEngine *engine, WgPeer *peer, const uint8_t *data,
		     size_t length)
{
    ssize_t sent;

    if (peer->endpoint_length == 0) {
	engine_log(engine, 1, "wg: no endpoint, packet dropped");
	return;
    }
    sent = sendto(engine->udp_fd, data, length, MSG_DONTWAIT,
		  (struct sockaddr *) &peer->endpoint,
		  peer->endpoint_length);
    if (sent < 0)
	engine_log(engine, 1, "wg: sendto failed: %s", strerror(errno));
    else if ((size_t) sent != length)
	engine_log(engine, 1, "wg: short send %zd/%zu", sent, length);
    else
	engine_log(engine, 5, "wg: sent %zu bytes", length);
}

static void handshake_peer(WgEngine *engine, WgPeer *peer)
{
    uint8_t message[WG_MSG_INITIATION_SIZE];
    uint32_t local = new_index();

    if (wg_noise_create_initiation(&peer->noise, local,
				   engine->config.private_key,
				   peer->config->public_key, message) < 0)
	return;
    peer->local_index = local;
    peer->handshake_pending = true;
    peer->last_initiation_ms = monotonic_ms();
    engine_log(engine, 4, "wg: handshake initiation sent");
    send_raw(engine, peer, message, sizeof(message));
}

void wg_engine_handshake(WgEngine *engine)
{
    int i;

    for (i = 0; i < engine->peer_count; i++) {
	if (!engine->peers[i].handshake_pending
	    && !engine->peers[i].have_session)
	    handshake_peer(engine, &engine->peers[i]);
    }
}

static WgPeer *peer_by_local_index(WgEngine *engine, uint32_t index)
{
    int i;

    for (i = 0; i < engine->peer_count; i++) {
	if (engine->peers[i].have_session
	    && engine->peers[i].local_index == index)
	    return &engine->peers[i];
    }
    return NULL;
}

static WgPeer *peer_by_pending_index(WgEngine *engine, uint32_t index)
{
    int i;

    for (i = 0; i < engine->peer_count; i++) {
	if (engine->peers[i].handshake_pending
	    && engine->peers[i].local_index == index)
	    return &engine->peers[i];
    }
    return NULL;
}

static WgPeer *peer_for_packet(WgEngine *engine, const uint8_t *packet,
			       size_t length)
{
    int i;

    if (length >= 20 && (packet[0] >> 4) == 4) {
	const uint8_t *destination = packet + 16;

	for (i = 0; i < engine->peer_count; i++) {
	    WgPeer *peer = &engine->peers[i];
	    int j;

	    for (j = 0; j < peer->config->allowed_count; j++) {
		WgAllowedIp *allowed = &peer->config->allowed[j];
		int bits;

		if (allowed->family != AF_INET)
		    continue;
		for (bits = 0; bits < allowed->prefix; bits++) {
		    int byte = bits / 8;
		    int bit = 7 - (bits % 8);

		    if (((allowed->address[byte] >> bit) & 1)
			!= ((destination[byte] >> bit) & 1))
			break;
		}
		if (bits == allowed->prefix)
		    return peer;
	    }
	}
    }

    return &engine->peers[0];
}

static void flush_queue(WgEngine *engine, WgPeer *peer);

static int send_data(WgEngine *engine, WgPeer *peer, const uint8_t *packet,
		     size_t length)
{
    uint8_t buffer[WG_DATA_HEADER_SIZE + WG_MAX_PACKET + 32];
    uint8_t plaintext[WG_MAX_PACKET + 32];
    size_t padded;
    int sealed;

    if (!peer->have_session) {
	if (length > 0 && peer->queue_count < WG_QUEUED_PACKETS) {
	    WgQueued *queued = &peer->queue[peer->queue_count++];

	    memcpy(queued->data, packet, length);
	    queued->length = length;
	}
	if (!peer->handshake_pending)
	    handshake_peer(engine, peer);
	return 0;
    }

    if (length > WG_MAX_PACKET)
	return -1;
    /* The reference implementation pads the plaintext to a multiple of
     * 16 bytes before encrypting; the receiver strips it with the IP
     * length.  */
    padded = (length + 15) & ~(size_t) 15;
    memcpy(plaintext, packet, length);
    if (padded > length)
	memset(plaintext + length, 0, padded - length);

    memset(buffer, 0, WG_DATA_HEADER_SIZE);
    buffer[0] = WG_MSG_DATA;
    put_le32(buffer + 4, peer->remote_index);
    put_le64(buffer + 8, peer->send_counter);

    /* The transport message authenticates nothing but its own nonce;
     * the 16-byte header is not additional data.  */
    sealed = wg_aead_encrypt(buffer + WG_DATA_HEADER_SIZE, peer->send_key,
			     peer->send_counter, plaintext, padded, NULL, 0);
    if (sealed < 0)
	return -1;
    send_raw(engine, peer, buffer, WG_DATA_HEADER_SIZE + (size_t) sealed);
    peer->send_counter++;
    peer->last_data_sent_ms = monotonic_ms();
    peer->keepalive_pending = false;
    return (int) length;
}

int wg_engine_send_ip(WgEngine *engine, const uint8_t *packet, size_t length)
{
    if (engine == NULL || length == 0 || length > WG_MAX_PACKET)
	return -1;
    return send_data(engine, peer_for_packet(engine, packet, length), packet,
		     length);
}

static void flush_queue(WgEngine *engine, WgPeer *peer)
{
    int i;

    for (i = 0; i < peer->queue_count; i++)
	send_data(engine, peer, peer->queue[i].data, peer->queue[i].length);
    peer->queue_count = 0;

    /* The initiator confirms the session with a keepalive; without it
     * the responder keeps the new keypair on the side and cannot send
     * anything back.  */
    if (peer->noise.initiator)
	send_data(engine, peer, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* Receive path                                                        */
/* ------------------------------------------------------------------ */

static int handle_initiation(WgEngine *engine, const uint8_t *packet,
			     size_t length, const struct sockaddr *source,
			     socklen_t source_length)
{
    WgPeer *peer = &engine->peers[0];
    uint8_t response[WG_MSG_RESPONSE_SIZE];
    uint8_t send_key[32];
    uint8_t recv_key[32];
    uint8_t initiator_static[32];
    uint32_t remote_index = 0;
    uint32_t local = new_index();

    if (length != WG_MSG_INITIATION_SIZE)
	return -1;
    if (wg_noise_consume_initiation(&peer->noise,
				    engine->config.private_key,
				    engine->config.public_key,
				    peer->config->preshared_key,
				    peer->config->has_preshared_key,
				    packet, response, local, &remote_index,
				    initiator_static, send_key,
				    recv_key) < 0) {
	engine_log(engine, 4, "wg: initiation rejected");
	return -1;
    }

    if (memcmp(initiator_static, peer->config->public_key, 32) != 0) {
	engine_log(engine, 4, "wg: initiation from an unknown peer");
	return -1;
    }

    /* Roaming: trust the source of a valid handshake.  */
    memcpy(&peer->endpoint, source, source_length);
    peer->endpoint_length = source_length;

    memcpy(peer->send_key, send_key, 32);
    memcpy(peer->recv_key, recv_key, 32);
    peer->local_index = local;
    peer->remote_index = remote_index;
    peer->send_counter = 0;
    peer->recv_last = 0;
    peer->recv_bitmap = 0;
    peer->recv_initialized = false;
    peer->have_session = true;
    peer->handshake_pending = false;
    peer->session_born_ms = monotonic_ms();

    send_raw(engine, peer, response, sizeof(response));
    engine_log(engine, 4, "wg: handshake accepted, response sent");
    flush_queue(engine, peer);
    return 0;
}

static int handle_response(WgEngine *engine, const uint8_t *packet,
			   size_t length, const struct sockaddr *source,
			   socklen_t source_length)
{
    WgPeer *peer;

    if (length != WG_MSG_RESPONSE_SIZE)
	return -1;
    peer = peer_by_pending_index(engine, get_le32(packet + 8));
    if (peer == NULL) {
	engine_log(engine, 4, "wg: response for an unknown handshake");
	return -1;
    }
    {
	int rc = wg_noise_consume_response(&peer->noise,
					   engine->config.private_key,
					   peer->config->preshared_key,
					   peer->config->has_preshared_key,
					   packet, peer->send_key,
					   peer->recv_key);
	if (rc < 0) {
	    engine_log(engine, 1, "wg: invalid handshake response rc=%d", rc);
	    return -1;
	}
    }

    memcpy(&peer->endpoint, source, source_length);
    peer->endpoint_length = source_length;
    peer->local_index = get_le32(packet + 8);
    peer->remote_index = get_le32(packet + 4);
    peer->send_counter = 0;
    peer->recv_last = 0;
    peer->recv_bitmap = 0;
    peer->recv_initialized = false;
    peer->have_session = true;
    peer->handshake_pending = false;
    peer->session_born_ms = monotonic_ms();

    engine_log(engine, 4, "wg: handshake complete");
    flush_queue(engine, peer);
    return 0;
}

static bool replay_ok(WgPeer *peer, uint64_t counter)
{
    if (!peer->recv_initialized) {
	peer->recv_initialized = true;
	peer->recv_last = counter;
	peer->recv_bitmap = 1;
	return true;
    }
    if (counter > peer->recv_last) {
	uint64_t shift = counter - peer->recv_last;

	if (shift >= 64)
	    peer->recv_bitmap = 0;
	else
	    peer->recv_bitmap = (peer->recv_bitmap << shift) | 1;
	peer->recv_last = counter;
	return true;
    }
    {
	uint64_t difference = peer->recv_last - counter;

	if (difference >= 64)
	    return false;
	if (peer->recv_bitmap & (1ULL << difference))
	    return false;
	peer->recv_bitmap |= 1ULL << difference;
	return true;
    }
}

static int handle_data(WgEngine *engine, const uint8_t *packet, size_t length,
		       const struct sockaddr *source, socklen_t source_length)
{
    WgPeer *peer;
    uint8_t plaintext[WG_MAX_PACKET];
    uint64_t counter;
    int decrypted;

    if (length < WG_DATA_HEADER_SIZE + 16)
	return -1;
    peer = peer_by_local_index(engine, get_le32(packet + 4));
    if (peer == NULL) {
	engine_log(engine, 4, "wg: data for an unknown session");
	return -1;
    }

    counter = get_le64(packet + 8);
    if (!replay_ok(peer, counter)) {
	engine_log(engine, 4, "wg: replayed data packet dropped");
	return -1;
    }

    decrypted = wg_aead_decrypt(plaintext, peer->recv_key, counter,
				packet + WG_DATA_HEADER_SIZE,
				length - WG_DATA_HEADER_SIZE, NULL, 0);
    if (decrypted < 0) {
	engine_log(engine, 4, "wg: data authentication failed");
	return -1;
    }
    /* Drop the padding using the IP header's total length.  */
    if (decrypted >= 20 && (plaintext[0] >> 4) == 4) {
	size_t ip_length = ((size_t) plaintext[2] << 8) | plaintext[3];

	if (ip_length >= 20 && ip_length <= (size_t) decrypted)
	    decrypted = (int) ip_length;
    } else if (decrypted >= 40 && (plaintext[0] >> 4) == 6) {
	size_t ip_length = ((size_t) plaintext[4] << 8) | plaintext[5];

	if (ip_length >= 40 && ip_length + 40 <= (size_t) decrypted)
	    decrypted = (int) (ip_length + 40);
    }

    memcpy(&peer->endpoint, source, source_length);
    peer->endpoint_length = source_length;
    peer->last_data_received_ms = monotonic_ms();
    peer->keepalive_pending = true;

    engine_log(engine, 4, "wg: data packet received (%d bytes)", decrypted);
    if (decrypted > 0 && engine->tcp != NULL
	&& wg_tcp_input(engine->tcp, plaintext, (size_t) decrypted))
	return 0;
    if (decrypted > 0 && engine->udp_forward != NULL
	&& wg_udp_input(engine->udp_forward, plaintext, (size_t) decrypted))
	return 0;

    if (decrypted > 0) {
	if (engine->callback != NULL)
	    engine->callback(engine->opaque, plaintext, (size_t) decrypted);
	else if (engine->tun_fd >= 0)
	    (void) write(engine->tun_fd, plaintext, (size_t) decrypted);
    }
    return 0;
}

static int udp_receive(WgEngine *engine)
{
    uint8_t packet[WG_MAX_PACKET];
    struct sockaddr_storage source;
    socklen_t source_length = sizeof(source);
    ssize_t length;

    length = recvfrom(engine->udp_fd, packet, sizeof(packet), 0,
		      (struct sockaddr *) &source, &source_length);
    if (length <= 0)
	return 0;

    switch (packet[0]) {
    case WG_MSG_INITIATION:
	return handle_initiation(engine, packet, (size_t) length,
				 (struct sockaddr *) &source,
				 source_length);
    case WG_MSG_RESPONSE:
	return handle_response(engine, packet, (size_t) length,
			       (struct sockaddr *) &source, source_length);
    case WG_MSG_COOKIE:
	/* Cookie replies protect against floods; a quiet tunnel never
	 * receives one, so it is simply ignored here.  */
	return 0;
    case WG_MSG_DATA:
	return handle_data(engine, packet, (size_t) length,
			   (struct sockaddr *) &source, source_length);
    default:
	return -1;
    }
}

static int tun_receive(WgEngine *engine)
{
    uint8_t packet[WG_MAX_PACKET];
    ssize_t length;

    if (engine->tun_fd < 0)
	return 0;
    length = read(engine->tun_fd, packet, sizeof(packet));
    if (length <= 0)
	return 0;
    return wg_engine_send_ip(engine, packet, (size_t) length);
}

static void timers(WgEngine *engine)
{
    uint64_t now = monotonic_ms();
    int i;

    for (i = 0; i < engine->peer_count; i++) {
	WgPeer *peer = &engine->peers[i];

	if (peer->queue_count > 0 && !peer->have_session
	    && !peer->handshake_pending)
	    handshake_peer(engine, peer);

	if (peer->handshake_pending
	    && now - peer->last_initiation_ms >= WG_REKEY_TIMEOUT_MS) {
	    if (now - peer->handshake_started_ms >= WG_REKEY_ATTEMPT_MS)
		peer->handshake_pending = false;
	    else {
		peer->last_initiation_ms = now;
		handshake_peer(engine, peer);
	    }
	}

	/* Keep the session alive, like the reference implementation:
	 * a keepalive is sent when data was received but nothing has
	 * been sent for a while, and the session is rekeyed when it is
	 * old enough.  */
	if (peer->have_session && peer->keepalive_pending
	    && now - peer->last_data_sent_ms >= WG_KEEPALIVE_MS)
	    send_data(engine, peer, NULL, 0);

	if (peer->have_session && peer->config->keepalive > 0
	    && now - peer->last_data_sent_ms
	    >= (uint64_t) peer->config->keepalive * 1000)
	    send_data(engine, peer, NULL, 0);

	if (peer->have_session
	    && now - peer->session_born_ms >= WG_REKEY_AFTER_MS) {
	    peer->session_born_ms = now;
	    handshake_peer(engine, peer);
	}
    }
}

int wg_engine_run_once(WgEngine *engine, int timeout_ms)
{
    struct pollfd descriptors[2 + 32];
    int count = 0;
    int tcp_count = 0;
    int udp_count = 0;
    int events = 0;
    int status;

    descriptors[count].fd = engine->udp_fd;
    descriptors[count].events = POLLIN;
    descriptors[count].revents = 0;
    count++;
    if (engine->tun_fd >= 0) {
	descriptors[count].fd = engine->tun_fd;
	descriptors[count].events = POLLIN;
	descriptors[count].revents = 0;
	count++;
    }

    if (engine->tcp != NULL) {
	int next;

	tcp_count = wg_tcp_pollfds(engine->tcp, descriptors + count,
				   (int) (sizeof(descriptors)
					  / sizeof(descriptors[0])) - count);
	next = wg_tcp_timeout(engine->tcp);
	if (next >= 0 && next < timeout_ms)
	    timeout_ms = next;
    }

    if (engine->udp_forward != NULL) {
	int base = count + tcp_count;

	udp_count = wg_udp_pollfds(engine->udp_forward,
				   descriptors + base,
				   (int) (sizeof(descriptors)
					  / sizeof(descriptors[0])) - base);
    }

    status = poll(descriptors, (nfds_t) (count + tcp_count + udp_count),
		  timeout_ms);
    if (status < 0 && errno != EINTR)
	return -1;
    if (status > 0) {
	if (descriptors[0].revents & POLLIN) {
	    if (udp_receive(engine) >= 0)
		events++;
	}
	if (count > 1 && (descriptors[1].revents & POLLIN)) {
	    if (tun_receive(engine) >= 0)
		events++;
	}
	if (tcp_count > 0)
	    wg_tcp_poll(engine->tcp, descriptors + count, tcp_count);
	if (udp_count > 0)
	    wg_udp_poll(engine->udp_forward, descriptors + count + tcp_count,
			udp_count);
    }

    timers(engine);
    if (engine->tcp != NULL)
	wg_tcp_tick(engine->tcp);
    if (engine->udp_forward != NULL)
	wg_udp_tick(engine->udp_forward);
    return events;
}

static void *engine_thread(void *argument)
{
    WgEngine *engine = argument;

    while (engine->running)
	(void) wg_engine_run_once(engine, 200);
    return NULL;
}

int wg_engine_start(WgEngine *engine)
{
    if (engine->running)
	return 0;
    engine->running = true;
    if (pthread_create(&engine->thread, NULL, engine_thread, engine) != 0) {
	engine->running = false;
	return -1;
    }
    return 0;
}

void wg_engine_stop(WgEngine *engine)
{
    if (engine == NULL || !engine->running)
	return;
    engine->running = false;
    pthread_join(engine->thread, NULL);
}
