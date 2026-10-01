#!/bin/sh
#
# SSH through the WireGuard bridge.  The host side is a real ssh client
# using the in-kernel WireGuard driver and TCP stack in an unprivileged
# network namespace; the container side is uvroot's user-space WireGuard
# engine with a TCP forward, relaying to an SSH server running inside the
# container.
#
# The server is a small pure-Python (paramiko) one instead of sshd: sshd
# always chroots and authenticates through PAM in its privilege
# separation child, which cannot work under an emulated root.  The point
# here is the network path -- real SSH over the tunnel.

if ! command -v wg >/dev/null 2>&1 || ! command -v unshare >/dev/null 2>&1 \
   || ! command -v ssh >/dev/null 2>&1 || ! command -v ssh-keygen >/dev/null 2>&1; then
    exit 125
fi
if [ ! -x "${UVROOT}" ]; then
    exit 125
fi
if ! python3 -c 'import paramiko' >/dev/null 2>&1; then
    exit 125
fi
if ! unshare -Ur -n sh -c 'ip link add wgprobe type wireguard' \
     >/dev/null 2>&1; then
    exit 125
fi

tmp=$(mktemp -d /tmp/uvroot-ssh.XXXXXX) || exit 125
trap 'rm -rf "${tmp}"' EXIT

ssh-keygen -t rsa -b 2048 -f "${tmp}/host_key" -N '' -q
ssh-keygen -t ed25519 -f "${tmp}/id" -N '' -q

cat > "${tmp}/server.py" <<'PY'
import socket, subprocess, sys, threading
import paramiko

host_key = paramiko.RSAKey.from_private_key_file(sys.argv[1])
authorized = open(sys.argv[2]).read().split()[1]
port = int(sys.argv[3])

class Server(paramiko.ServerInterface):
    def check_auth_publickey(self, username, key):
        return (paramiko.AUTH_SUCCESSFUL if key.get_base64() == authorized
                else paramiko.AUTH_FAILED)
    def get_allowed_auths(self, username):
        return 'publickey'
    def check_channel_request(self, kind, chanid):
        return paramiko.OPEN_SUCCEEDED
    def check_channel_exec_request(self, channel, command):
        threading.Thread(target=self.run, args=(channel, command),
                         daemon=True).start()
        return True
    def run(self, channel, command):
        try:
            # Executing the client's command is what an SSH exec request
            # means; the shell is explicit and this server only ever runs
            # inside a throwaway test container.
            done = subprocess.run(['/bin/sh', '-c', command],
                                  capture_output=True, timeout=20)
            channel.sendall(done.stdout)
            if done.stderr:
                channel.sendall_stderr(done.stderr)
            channel.send_exit_status(done.returncode)
        except Exception as error:
            channel.sendall_stderr(str(error).encode())
            channel.send_exit_status(1)
        channel.close()

sock = socket.socket()
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(('127.0.0.1', port))
sock.listen(5)
while True:
    client, _ = sock.accept()
    transport = paramiko.Transport(client)
    transport.add_server_key(host_key)
    try:
        transport.start_server(server=Server())
    except Exception:
        transport.close()
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
forward=10.9.0.2:22=127.0.0.1:2222"

# The SSH server runs inside the container (as the uvroot guest).
"$uvroot" -r / --net --net-bridge=userspace --wg="vtun:$config" \
    python3 "$TMP/server.py" "$TMP/host_key" "$TMP/id.pub" 2222 \
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

ssh -F /dev/null -p 22 -i "$TMP/id" -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null -o BatchMode=yes user@10.9.0.2 \
    'echo SSH_OVER_WG_OK' 2>/dev/null || true

pkill -9 -f "$TMP/server.py" 2>/dev/null || true
kill -9 "$server_pid" 2>/dev/null || true
exit 0
SCRIPT

result=$(TMP="${tmp}" timeout 150 unshare -Ur -n sh "${tmp}/run.sh" \
	 "${UVROOT}" 2>&1 | tail -1)
case "${result}" in
*SSH_OVER_WG_OK*) exit 0 ;;
esac
echo "SSH over WireGuard failed: ${result}" >&2
exit 1
