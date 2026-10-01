#!/bin/sh
#
# Privileged port emulation for a virtual root: -i 0:0 lets the guest
# bind(2) to ports below 1024, and a loopback client reaches it through
# the emulated port, while a plain user still cannot bind them.

if [ ! -x "${UVROOT}" ] || ! command -v python3 >/dev/null 2>&1; then
    exit 125
fi

result=$("${UVROOT}" -r / -i 0:0 python3 -c "
import socket
for port in (80, 22):
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('127.0.0.1', port))
    server.listen(1)
    print('bound=%d' % server.getsockname()[1])
    client = socket.socket()
    client.connect(('127.0.0.1', port))
    connection, _ = server.accept()
    print('reached=%d accepted=%d' % (port, connection.getsockname()[1]))
    connection.close()
    client.close()
    server.close()
" 2>&1)

echo "${result}"

for port in 80 22; do
    case "${result}" in
    *"bound=${port}"*) ;;
    *) echo "port ${port} could not be created" >&2; exit 1 ;;
    esac
    case "${result}" in
    *"reached=${port} accepted=${port}"*) ;;
    *) echo "port ${port} is not reachable on loopback" >&2; exit 1 ;;
    esac
done

if "${UVROOT}" -r / python3 -c "
import socket
socket.socket().bind(('127.0.0.1', 80))
" >/dev/null 2>&1; then
    echo "a plain user managed to bind port 80" >&2
    exit 1
fi
