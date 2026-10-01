#!/bin/sh
#
# WireGuard interoperability: the user-space engine speaks the real
# protocol to the in-kernel WireGuard driver.  It is run inside an
# unprivileged user+network namespace (the only way an unprivileged
# process can create a kernel WireGuard interface), and skipped when the
# kernel or the namespace support is missing.

if ! command -v wg >/dev/null 2>&1 || ! command -v unshare >/dev/null 2>&1 \
   || ! command -v gcc >/dev/null 2>&1; then
    exit 125
fi
if [ ! -r /usr/include/openssl/evp.h ]; then
    exit 125
fi
if ! unshare -Ur -n true >/dev/null 2>&1; then
    exit 125
fi
if ! unshare -Ur -n sh -c 'ip link add wgprobe type wireguard' \
     >/dev/null 2>&1; then
    exit 125
fi

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "${here}/.." && pwd)
wgdir="${root}/src/extension/netvirt/wg"
tmp=$(mktemp -d /tmp/uvroot-wgk.XXXXXX) || exit 125
trap 'rm -rf "${tmp}"' EXIT

gcc -O1 -DHAVE_LIBCRYPTO -I"${wgdir}" -I"${root}/src" -o "${tmp}/wg-peer" \
    "${here}/wg-interop-peer.c" "${wgdir}/wg_crypto.c" \
    "${wgdir}/wg_noise.c" "${wgdir}/wg_engine.c" \
    "${wgdir}/wg_tcp.c" "${wgdir}/wg_udp.c" -ldl -lpthread \
    2>/dev/null || exit 125

cat > "${tmp}/interop.sh" <<'SCRIPT'
set -e
PEER="$1"
KA=$(wg genkey); PA=$(printf %s "$KA" | wg pubkey)
KB=$(wg genkey); PB=$(printf %s "$KB" | wg pubkey)
printf '%s' "$KA" > /tmp/uvroot-wgk-ka
ip link set lo up

# --- the kernel initiates, the user-space engine responds -------------
ip link add wg0 type wireguard
ip addr add 10.9.0.1/24 dev wg0
ip link set wg0 up
wg set wg0 private-key /tmp/uvroot-wgk-ka listen-port 51820 peer "$PB" \
   endpoint 127.0.0.1:51821 allowed-ips 10.9.0.2/32
CFG="private_key=$KB
listen_port=51821
public_key=$PA
endpoint=127.0.0.1:51820
allowed_ip=10.9.0.1/32"
"$PEER" "$CFG" --count 2 > /tmp/uvroot-wgk-fwd.out 2>/dev/null &
PEER_PID=$!
sleep 0.5
ping -c2 -W3 -i0.4 10.9.0.2 >/dev/null 2>&1 && FWD=1 || FWD=0
wait $PEER_PID || true
ip link del wg0

# --- the user-space engine initiates, the kernel responds -------------
ip link add wg1 type wireguard
ip addr add 10.9.0.1/24 dev wg1
ip link set wg1 up
wg set wg1 private-key /tmp/uvroot-wgk-ka listen-port 51820 peer "$PB" \
   allowed-ips 10.9.0.2/32
"$PEER" "$CFG" --initiate --count 2 > /tmp/uvroot-wgk-rev.out 2>/dev/null &
PEER_PID=$!
sleep 0.6
ping -c2 -W3 -i0.4 10.9.0.2 >/dev/null 2>&1 && REV=1 || REV=0
wait $PEER_PID || true

echo "FWD=$FWD REV=$REV"
SCRIPT

result=$(timeout 90 unshare -Ur -n sh "${tmp}/interop.sh" \
	 "${tmp}/wg-peer" 2>&1 | tail -1)
case "${result}" in
*"FWD=1 REV=1"*) exit 0 ;;
esac
echo "kernel WireGuard interoperability failed: ${result}" >&2
exit 1
