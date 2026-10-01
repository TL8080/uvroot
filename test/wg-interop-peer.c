/*
 * A minimal host-side WireGuard peer used by the tests: it terminates a
 * tunnel and answers ICMP echo requests, so a kernel WireGuard interface
 * (or another instance of this peer) can ping it.  The configuration is
 * the same wg(8)/flat text the bridge accepts.
 *
 * Usage: wg-interop-peer <config> [--initiate]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wg_engine.h"

static int answered = 0;
static int limit = 2;
static WgEngine *global_engine = NULL;

static void log_callback(void *opaque, int level, const char *message)
{
    (void) opaque;
    fprintf(stderr, "wg[%d] %s\n", level, message);
}

static uint16_t checksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i + 1 < length; i += 2)
	sum += ((uint32_t) data[i] << 8) | data[i + 1];
    if (i < length)
	sum += (uint32_t) data[i] << 8;
    while (sum >> 16)
	sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t) ~sum;
}

static void on_packet(void *opaque, const uint8_t *packet, size_t length)
{
    uint8_t reply[WG_MAX_PACKET];
    size_t header_length;
    size_t total_length;
    (void) opaque;

    if (length < 20 || (packet[0] >> 4) != 4)
	return;
    header_length = (size_t) (packet[0] & 0x0F) * 4;
    if (length < header_length + 8)
	return;
    if (packet[9] != 1)		/* not ICMP */
	return;
    if (packet[header_length] != 8)	/* not an echo request */
	return;

    total_length = ((size_t) packet[2] << 8) | packet[3];
    if (total_length > length || total_length < header_length + 8)
	total_length = length;
    memcpy(reply, packet, total_length);

    /* Swap the addresses so the reply goes back.  */
    {
	uint8_t temporary[4];

	memcpy(temporary, reply + 12, 4);
	memcpy(reply + 12, reply + 16, 4);
	memcpy(reply + 16, temporary, 4);
    }
    reply[10] = 0;
    reply[11] = 0;
    {
	uint16_t value = checksum(reply, header_length);

	reply[10] = (uint8_t) (value >> 8);
	reply[11] = (uint8_t) value;
    }
    reply[header_length] = 0;	/* echo reply */
    reply[header_length + 1] = 0;
    reply[header_length + 2] = 0;
    reply[header_length + 3] = 0;
    {
	uint16_t value = checksum(reply + header_length,
				  total_length - header_length);

	reply[header_length + 2] = (uint8_t) (value >> 8);
	reply[header_length + 3] = (uint8_t) value;
    }

    wg_engine_send_ip(global_engine, reply, total_length);
    answered++;
}

int main(int argc, char **argv)
{
    WgEngineConfig config;
    WgEngine *engine;
    char error[256];
    int i;

    if (argc < 2) {
	fprintf(stderr, "usage: %s <config> [--initiate] [--count N]\n",
		argv[0]);
	return 2;
    }
    if (argc > 3) {
	if (strcmp(argv[2], "--count") == 0)
	    limit = atoi(argv[3]);
	else if (argc > 2 && strcmp(argv[2], "--initiate") == 0) {
	    /* handled below */
	}
    }

    if (wg_engine_config_parse(&config, argv[1], error, sizeof(error)) < 0) {
	fprintf(stderr, "config: %s\n", error);
	return 1;
    }
    engine = wg_engine_new(&config, -1, on_packet, NULL, error, sizeof(error));
    if (engine == NULL) {
	fprintf(stderr, "engine: %s\n", error);
	return 1;
    }
    global_engine = engine;
    wg_engine_set_log(engine, log_callback, NULL);

    if (argc > 2 && strcmp(argv[2], "--initiate") == 0)
	wg_engine_handshake(engine);

    for (i = 0; i < 400; i++) {	/* up to 20 seconds */
	wg_engine_run_once(engine, 50);
	if (limit == 0) {
	    if (wg_engine_ready(engine))
		break;
	} else if (answered >= limit)
	    break;
    }

    printf("answered=%d ready=%d\n", answered,
	   wg_engine_ready(engine) ? 1 : 0);
    fflush(stdout);

    wg_engine_free(engine);
    wg_engine_config_free(&config);
    return answered >= limit ? 0 : 1;
}
