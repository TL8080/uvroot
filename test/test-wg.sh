#!/bin/sh
#
# The built-in user-space WireGuard engine.  A host-side peer and a
# container agree on a WireGuard tunnel; the container creates a virtual
# /dev/net/tun, configures it and gets an ICMP echo reply through the
# tunnel.  No privilege, kernel module or /dev/net/tun access is used.

if [ ! -x "${UVROOT}" ]; then
    exit 125
fi
if ! command -v wg >/dev/null 2>&1 || ! command -v gcc >/dev/null 2>&1; then
    exit 125
fi
if [ ! -r /usr/include/openssl/evp.h ]; then
    exit 125
fi

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "${here}/.." && pwd)
wgdir="${root}/src/extension/netvirt/wg"

tmp=$(mktemp -d /tmp/uvroot-wg.XXXXXX) || exit 125
trap 'rm -rf "${tmp}"' EXIT

# The engine takes X25519 and ChaCha20-Poly1305 from libcrypto at run
# time, so the helper only needs the headers at build time.
gcc -O1 -DHAVE_LIBCRYPTO -I"${wgdir}" -I"${root}/src" -o "${tmp}/wg-peer" \
    "${here}/wg-interop-peer.c" "${wgdir}/wg_crypto.c" \
    "${wgdir}/wg_noise.c" "${wgdir}/wg_engine.c" \
    "${wgdir}/wg_tcp.c" "${wgdir}/wg_udp.c" -ldl -lpthread \
    2>/dev/null || exit 125
gcc -O1 -o "${tmp}/tun-ping" "${here}/netvirt-tun-ping.c" \
    2>/dev/null || exit 125

host_private=$(wg genkey)
host_public=$(printf %s "${host_private}" | wg pubkey)
container_private=$(wg genkey)
container_public=$(printf %s "${container_private}" | wg pubkey)

host_config="private_key=${host_private}
listen_port=51820
public_key=${container_public}
endpoint=127.0.0.1:51821
allowed_ip=10.9.0.2/32"

container_config="private_key=${container_private}
listen_port=51821
public_key=${host_public}
endpoint=127.0.0.1:51820
allowed_ip=10.9.0.1/32"

"${tmp}/wg-peer" "${host_config}" --count 1 \
    > "${tmp}/host.out" 2> "${tmp}/host.err" &
host_pid=$!
sleep 0.3

"${UVROOT}" -r / --net --net-bridge=userspace \
    --wg="vtun:${container_config}" \
    "${tmp}/tun-ping" 10.9.0.2 10.9.0.1
status=$?

wait "${host_pid}" 2>/dev/null || true

if [ "${status}" -ne 0 ]; then
    echo "container could not ping through the tunnel" >&2
    echo "host peer: $(cat "${tmp}/host.out")" >&2
    exit 1
fi
grep -q 'answered=1' "${tmp}/host.out" || {
    echo "host peer did not answer" >&2
    exit 1
}
