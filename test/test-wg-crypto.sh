#!/bin/sh
#
# Known-answer checks for the WireGuard primitives and the handshake key
# schedule: BLAKE2s, HMAC-BLAKE2s and the WireGuard KDF against Python's
# hashlib/hmac, X25519 against RFC 7748, and an in-process handshake
# between the initiator and the responder.

if ! command -v gcc >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then
    exit 125
fi
if [ ! -r /usr/include/openssl/evp.h ]; then
    exit 125
fi

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "${here}/.." && pwd)
wgdir="${root}/src/extension/netvirt/wg"
tmp=$(mktemp -d /tmp/uvroot-wgc.XXXXXX) || exit 125
trap 'rm -rf "${tmp}"' EXIT

gcc -O1 -DHAVE_LIBCRYPTO -I"${wgdir}" -o "${tmp}/crypto" \
    "${here}/wg-crypto-test.c" "${wgdir}/wg_crypto.c" -ldl \
    2>/dev/null || exit 125
gcc -O1 -DHAVE_LIBCRYPTO -I"${wgdir}" -o "${tmp}/noise" \
    "${here}/wg-noise-test.c" "${wgdir}/wg_crypto.c" \
    "${wgdir}/wg_noise.c" -ldl 2>/dev/null || exit 125

"${tmp}/crypto" > "${tmp}/crypto.out" 2>&1 || {
    echo "crypto self-test failed" >&2
    exit 1
}

python3 - "${tmp}/crypto.out" <<'PY' || exit 1
import hashlib, hmac, sys
values = {}
for line in open(sys.argv[1]):
    if '=' in line:
        key, value = line.strip().split('=', 1)
        values[key] = value
errors = 0
def check(name, expected):
    global errors
    if values.get(name) != expected:
        print("%s: %s != %s" % (name, values.get(name), expected), file=sys.stderr)
        errors += 1
check('blake2s_abc', hashlib.blake2s(b'abc').hexdigest())
check('blake2s_keyed', hashlib.blake2s(b'msg', digest_size=16, key=b'key').hexdigest())
check('hmac_blake2s', hmac.new(b'key', b'msg', hashlib.blake2s).hexdigest())
check('x25519_alice_public',
      '8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a')
check('x25519_shared',
      '4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742')
for name in ('aead_roundtrip', 'aead_tamper_rejected'):
    check(name, '1')
key = bytes(range(32))
t0 = hmac.new(key, b'input', hashlib.blake2s).digest()
t1 = hmac.new(t0, b'\x01', hashlib.blake2s).digest()
t2 = hmac.new(t0, t1 + b'\x02', hashlib.blake2s).digest()
t3 = hmac.new(t0, t2 + b'\x03', hashlib.blake2s).digest()
check('kdf_1', t1.hex())
check('kdf_2', t2.hex())
check('kdf_3', t3.hex())
if errors:
    sys.exit(1)
PY

"${tmp}/noise" > "${tmp}/noise.out" 2>&1 || {
    echo "handshake self-test failed:" >&2
    cat "${tmp}/noise.out" >&2
    exit 1
}
grep -q 'handshake_test=OK' "${tmp}/noise.out" || {
    echo "handshake self-test did not agree on the keys" >&2
    exit 1
}
