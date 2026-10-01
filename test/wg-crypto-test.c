/*
 * Self-test for the WireGuard crypto layer.  It is not a uvroot test
 * (see test-netvirt.sh); the shell wrapper compares its output with
 * reference values computed by Python's hashlib/hmac, RFC 7748 and
 * RFC 8439.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wg_crypto.h"

static void dump(const char *label, const uint8_t *data, size_t length)
{
    size_t i;

    printf("%s=", label);
    for (i = 0; i < length; i++)
	printf("%02x", data[i]);
    printf("\n");
}

static int from_hex(const char *text, uint8_t *out, size_t length)
{
    size_t i;

    for (i = 0; i < length; i++) {
	unsigned int value;

	if (sscanf(text + 2 * i, "%2x", &value) != 1)
	    return -1;
	out[i] = (uint8_t) value;
    }
    return 0;
}

int main(void)
{
    uint8_t digest[32];
    uint8_t keyed[16];
    uint8_t mac[32];
    uint8_t output[3][32];
    uint8_t alice_private[32];
    uint8_t alice_public[32];
    uint8_t bob_private[32];
    uint8_t bob_public[32];
    uint8_t shared[32];
    uint8_t aead_key[32];
    uint8_t aead_aad[12];
    uint8_t plain[114];
    uint8_t sealed[256];
    uint8_t opened[256];
    int sealed_length;
    int opened_length;

    printf("crypto_ready=%d\n", wg_crypto_init() ? 1 : 0);
    if (!wg_crypto_ready()) {
	printf("crypto_error=%s\n", wg_crypto_error());
	return 1;
    }

    wg_blake2s(digest, 32, "abc", 3, NULL, 0);
    dump("blake2s_abc", digest, 32);

    wg_blake2s(keyed, 16, "msg", 3, "key", 3);
    dump("blake2s_keyed", keyed, 16);

    wg_hmac_blake2s(mac, (const uint8_t *) "key", 3, "msg", 3);
    dump("hmac_blake2s", mac, 32);

    wg_kdf(output, 3, (const uint8_t *)
	   "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
	   "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f",
	   "input", 5);
    dump("kdf_1", output[0], 32);
    dump("kdf_2", output[1], 32);
    dump("kdf_3", output[2], 32);

    /* RFC 7748 section 6.1.  */
    from_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a",
	     alice_private, 32);
    from_hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb",
	     bob_private, 32);
    wg_x25519_public(alice_public, alice_private);
    wg_x25519_public(bob_public, bob_private);
    dump("x25519_alice_public", alice_public, 32);
    dump("x25519_bob_public", bob_public, 32);
    wg_x25519(shared, alice_private, bob_public);
    dump("x25519_shared", shared, 32);

    /* ChaCha20-Poly1305 round-trip (the exact nonce layout is validated
     * against the kernel implementation by test-wg-interop).  */
    memset(aead_key, 0x42, sizeof(aead_key));
    memset(aead_aad, 0x24, sizeof(aead_aad));
    memset(plain, 0x11, sizeof(plain));
    sealed_length = wg_aead_encrypt(sealed, aead_key, 7, plain,
				    sizeof(plain), aead_aad,
				    sizeof(aead_aad));
    printf("aead_sealed_length=%d\n", sealed_length);
    opened_length = wg_aead_decrypt(opened, aead_key, 7, sealed,
				    (size_t) sealed_length, aead_aad,
				    sizeof(aead_aad));
    printf("aead_opened_length=%d\n", opened_length);
    printf("aead_roundtrip=%d\n",
	   opened_length == (int) sizeof(plain)
	   && memcmp(opened, plain, sizeof(plain)) == 0);
    /* A tampered tag must be rejected.  */
    sealed[sealed_length - 1] ^= 0x01;
    printf("aead_tamper_rejected=%d\n",
	   wg_aead_decrypt(opened, aead_key, 7, sealed,
			   (size_t) sealed_length, aead_aad,
			   sizeof(aead_aad)) < 0);

    printf("tai64n=");
    {
	uint8_t stamp[12];
	size_t i;

	wg_tai64n(stamp);
	for (i = 0; i < 12; i++)
	    printf("%02x", stamp[i]);
	printf("\n");
    }

    return 0;
}
