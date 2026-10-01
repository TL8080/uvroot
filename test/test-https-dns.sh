#!/bin/sh
#
# HTTPS and DNS through the WireGuard bridge.  A real curl and a real
# dig, on the host side of the tunnel (in-kernel WireGuard and TCP/UDP
# stacks in an unprivileged netns), reach services inside the container:
# HTTPS on TCP 443 (relayed by the TCP terminator) and DNS on port 53,
# both the usual UDP transport and the TCP fallback.

for tool in wg unshare curl dig openssl python3 ssh-keygen; do
    command -v "${tool}" >/dev/null 2>&1 || exit 125
done
[ -x "${UVROOT}" ] || exit 125
if ! unshare -Ur -n sh -c 'ip link add wgprobe type wireguard' \
     >/dev/null 2>&1; then
    exit 125
fi

tmp=$(mktemp -d /tmp/uvroot-netsvc.XXXXXX) || exit 125
trap 'rm -rf "${tmp}"' EXIT

openssl req -x509 -newkey rsa:2048 -nodes -keyout "${tmp}/key.pem" \
    -out "${tmp}/cert.pem" -days 1 -subj /CN=10.9.0.2 >/dev/null 2>&1 \
    || exit 125

cat > "${tmp}/server.py" <<'PY'
import http.server, socket, ssl, sys, threading

certificate, key = sys.argv[1], sys.argv[2]

def dns_response(query):
    if len(query) < 12:
        return None
    index = 12
    while index < len(query) and query[index] != 0:
        index += 1 + query[index]
    index += 1 + 4
    question = query[12:index]
    header = (query[:2] + b'\x81\x80' + b'\x00\x01' + b'\x00\x01'
              + b'\x00\x00' + b'\x00\x00')
    answer = (b'\xc0\x0c' + b'\x00\x01' + b'\x00\x01'
              + b'\x00\x00\x00\x3c' + b'\x00\x04' + bytes([10, 9, 0, 9]))
    return header + question + answer

def udp_dns():
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('127.0.0.1', 5353))
    while True:
        data, address = server.recvfrom(2048)
        reply = dns_response(data)
        if reply:
            server.sendto(reply, address)

def tcp_dns():
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('127.0.0.1', 5354))
    server.listen(5)
    while True:
        connection, _ = server.accept()
        try:
            length = int.from_bytes(connection.recv(2), 'big')
            query = connection.recv(length)
            reply = dns_response(query)
            if reply:
                connection.sendall(len(reply).to_bytes(2, 'big') + reply)
        finally:
            connection.close()

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'HTTPS_OVER_WG_OK\n'
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *arguments):
        pass

def https():
    server = http.server.HTTPServer(('127.0.0.1', 8443), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certificate, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    server.serve_forever()

for service in (udp_dns, tcp_dns, https):
    threading.Thread(target=service, daemon=True).start()
threading.Event().wait()
PY

cat > "${tmp}/run.sh" <<'SCRIPT'
set -e
uvroot="$1"
ip link set lo up

kernel_private=$(wg genkey)
kernel_public=$(printf %s "$kernel_private" | wg pubkey)
container_private=$(wg genkey)
container_public=$(printf %s "$container_private" | wg pubkey)
printf '%s' "$kernel_private" > "$TMP/kernel.key"

config="private_key=$container_private
listen_port=51821
public_key=$kernel_public
endpoint=127.0.0.1:51820
allowed_ip=10.9.0.1/32
forward=10.9.0.2:443=127.0.0.1:8443
forward=10.9.0.2:53=127.0.0.1:5354
forward_udp=10.9.0.2:53=127.0.0.1:5353"

"$uvroot" -r / --net --net-bridge=userspace --wg="vtun:$config" \
    python3 "$TMP/server.py" "$TMP/cert.pem" "$TMP/key.pem" \
    > "$TMP/server.log" 2>&1 &
server_pid=$!
sleep 1.5

ip link add wg0 type wireguard
ip addr add 10.9.0.1/24 dev wg0
wg set wg0 private-key "$TMP/kernel.key" listen-port 51820 \
    peer "$container_public" endpoint 127.0.0.1:51821 \
    allowed-ips 10.9.0.2/32
ip link set wg0 up
sleep 0.5

echo "https=$(timeout 15 curl -sk https://10.9.0.2/ | tr -d '\n')"
echo "udp=$(timeout 15 dig +time=3 +tries=1 @10.9.0.2 example.com A +short | head -1)"
echo "tcp=$(timeout 15 dig +tcp +time=3 +tries=1 @10.9.0.2 example.com A +short | head -1)"

# uvroot forwards SIGTERM to its guest, so kill the guest and uvroot
# outright: the guest is the process whose argument is this server path.
pkill -9 -f "$TMP/server.py" 2>/dev/null || true
kill -9 "$server_pid" 2>/dev/null || true
exit 0
SCRIPT

TMP="${tmp}" timeout 180 unshare -Ur -n sh "${tmp}/run.sh" \
    "${UVROOT}" > "${tmp}/out" 2>&1 || true
result=$(cat "${tmp}/out")

echo "${result}" | grep -E '^(https|udp|tcp)=' || true
case "${result}" in
*"https=HTTPS_OVER_WG_OK"*) ;;
*) echo "HTTPS over WireGuard failed" >&2; exit 1 ;;
esac
case "${result}" in
*"udp=10.9.0.9"*) ;;
*) echo "DNS over UDP through WireGuard failed" >&2; exit 1 ;;
esac
case "${result}" in
*"tcp=10.9.0.9"*) ;;
*) echo "DNS over TCP through WireGuard failed" >&2; exit 1 ;;
esac
