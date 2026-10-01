#!/bin/sh
#
# A TCP service inside the container, reached through the WireGuard
# bridge.  The host side is the in-kernel driver in an unprivileged
# network namespace (the only way an unprivileged process gets a real
# TCP client), and the container side is uvroot's user-space TCP
# terminator: the tunnel carries TCP to the container's address and the
# stream is relayed to a local socket.

if ! command -v wg >/dev/null 2>&1 || ! command -v unshare >/dev/null 2>&1 \
   || ! command -v python3 >/dev/null 2>&1; then
    exit 125
fi
if [ ! -x "${UVROOT}" ] || [ ! -r /usr/include/openssl/evp.h ]; then
    exit 125
fi
if ! unshare -Ur -n sh -c 'ip link add wgprobe type wireguard' \
     >/dev/null 2>&1; then
    exit 125
fi

tmp=$(mktemp -d /tmp/uvroot-wgtcp.XXXXXX) || exit 125
trap 'rm -rf "${tmp}"' EXIT

cat > "${tmp}/run.sh" <<'SCRIPT'
set -e
UVROOT="$1"
ip link set lo up

kernel_private=$(wg genkey)
kernel_public=$(printf %s "$kernel_private" | wg pubkey)
container_private=$(wg genkey)
container_public=$(printf %s "$container_private" | wg pubkey)
printf '%s' "$kernel_private" > "$TMP/kernel.key"

# The service the tunnel exposes; it echoes what it receives.
python3 -c "
import socket
server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('127.0.0.1', 2222))
server.listen(5)
while True:
    connection, _ = server.accept()
    data = connection.recv(200)
    connection.sendall(b'ECHO:' + data)
    connection.close()
" > "$TMP/service.log" 2>&1 &
service_pid=$!
sleep 0.5

config="private_key=$container_private
listen_port=51821
public_key=$kernel_public
endpoint=127.0.0.1:51820
allowed_ip=10.9.0.1/32
forward=10.9.0.2:22=127.0.0.1:2222"

"$UVROOT" -r / --net --net-bridge=userspace --wg="vtun:$config" \
    /bin/sleep 20 > "$TMP/uvroot.log" 2>&1 &
uvroot_pid=$!
sleep 0.6

ip link add wg0 type wireguard
ip addr add 10.9.0.1/24 dev wg0
wg set wg0 private-key "$TMP/kernel.key" listen-port 51820 \
    peer "$container_public" endpoint 127.0.0.1:51821 \
    allowed-ips 10.9.0.2/32
ip link set wg0 up
sleep 0.5

timeout 10 python3 -c "
import socket
client = socket.socket()
client.settimeout(8)
client.connect(('10.9.0.2', 22))
client.sendall(b'through-the-tunnel')
print(client.recv(200).decode())
client.close()
" || true

kill "$uvroot_pid" "$service_pid" 2>/dev/null || true
wait 2>/dev/null || true
SCRIPT

result=$(TMP="${tmp}" timeout 90 unshare -Ur -n sh "${tmp}/run.sh" \
	 "${UVROOT}" 2>&1 | tail -1)
case "${result}" in
*"ECHO:through-the-tunnel"*) exit 0 ;;
esac
echo "TCP over WireGuard failed: ${result}" >&2
exit 1
