/*
 * Self-test for the WireGuard handshake and transport keys.  It runs an
 * initiator and a responder in the same process and checks that the two
 * sides agree on the transport keys, then exchanges a data message both
 * ways.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wg_crypto.h"
#include "wg_noise.h"

static int check(const char *label, int condition)
{
    printf("%s=%d\n", label, condition ? 1 : 0);
    return condition ? 0 : 1;
}

int main(void)
{
    uint8_t initiator_private[32];
    uint8_t initiator_public[32];
    uint8_t responder_private[32];
    uint8_t responder_public[32];
    uint8_t message1[WG_MSG_INITIATION_SIZE];
    uint8_t message2[WG_MSG_RESPONSE_SIZE];
    uint8_t initiator_send[32];
    uint8_t initiator_recv[32];
    uint8_t responder_send[32];
    uint8_t responder_recv[32];
    uint8_t responder_static_seen[32];
    uint32_t responder_seen_index = 0;
    WgNoise initiator;
    WgNoise responder;
    int failures = 0;

    memset(&initiator, 0, sizeof(initiator));
    memset(&responder, 0, sizeof(responder));

    if (!wg_crypto_init()) {
	printf("crypto_error=%s\n", wg_crypto_error());
	return 1;
    }

    wg_random(initiator_private, 32);
    wg_x25519_public(initiator_public, initiator_private);
    wg_random(responder_private, 32);
    wg_x25519_public(responder_public, responder_private);

    if (wg_noise_create_initiation(&initiator, 0x11111111,
				   initiator_private, responder_public,
				   message1) < 0) {
	printf("create_initiation=failed\n");
	return 1;
    }
    printf("initiation_type=%u\n", message1[0]);

    {
	int rc = wg_noise_consume_initiation(&responder, responder_private,
					     responder_public, NULL, false,
					     message1, message2, 0x22222222,
					     &responder_seen_index,
					     responder_static_seen,
					     responder_send, responder_recv);
	if (rc < 0) {
	    printf("consume_initiation=failed rc=%d\n", rc);
	    return 1;
	}
    }
    printf("response_type=%u\n", message2[0]);

    {
	int rc = wg_noise_consume_response(&initiator, initiator_private, NULL,
					   false, message2, initiator_send,
					   initiator_recv);
	if (rc < 0) {
	    printf("consume_response=failed rc=%d\n", rc);
	    return 1;
	}
    }

    failures += check("initiator_static_seen",
		      memcmp(responder_static_seen, initiator_public, 32) == 0);
    failures += check("seen_index", responder_seen_index == 0x11111111);
    failures += check("keys_i_send_eq_r_recv",
		      memcmp(initiator_send, responder_recv, 32) == 0);
    failures += check("keys_i_recv_eq_r_send",
		      memcmp(initiator_recv, responder_send, 32) == 0);

    /* Data message initiator -> responder.  */
    {
	uint8_t header[WG_DATA_HEADER_SIZE];
	uint8_t payload[48];
	uint8_t opened[32];
	uint8_t plain[32];
	int sealed;
	int length;
	uint32_t receiver = initiator.remote_index;

	memset(plain, 0x5a, sizeof(plain));
	memset(header, 0, sizeof(header));
	header[0] = WG_MSG_DATA;
	header[4] = (uint8_t) receiver;
	header[5] = (uint8_t) (receiver >> 8);
	header[6] = (uint8_t) (receiver >> 16);
	header[7] = (uint8_t) (receiver >> 24);
	/* counter 0 */
	sealed = wg_aead_encrypt(payload, initiator_send, 0, plain,
				 sizeof(plain), header, sizeof(header));
	failures += check("data_sealed_length", sealed == 48);
	length = wg_aead_decrypt(opened, responder_recv, 0, payload, 48,
				 header, sizeof(header));
	failures += check("data_decrypted_length", length == 32);
	failures += check("data_decrypted_content",
			  length == 32 && memcmp(opened, plain, 32) == 0);
    }

    printf("handshake_test=%s\n", failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
