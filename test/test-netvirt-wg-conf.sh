#!/bin/sh
# The built-in WireGuard engine must receive the body of a conf= file,
# not the literal "conf=/path" string.  A regression here falls back to
# NAT and never opens the configured UDP listener.
set -u

UVROOT="${UVROOT:-$(cd "$(dirname "$0")/.." && pwd)/src/uvroot}"
R="$(mktemp -d)"
PID=""
pass=0
fail=0

cleanup() {
    if [ -n "$PID" ]; then
        kill "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    rm -rf "$R"
}
trap cleanup EXIT INT TERM

ok() {
    if [ "$2" = "$3" ]; then
        echo "  ok   $1"
        pass=$((pass + 1))
    else
        echo "  FAIL $1 (got [$2] want [$3])"
        fail=$((fail + 1))
    fi
}

command -v wg >/dev/null 2>&1 || {
    echo "skip: wg is not installed"
    exit 0
}

PORT=51917
while ss -lun 2>/dev/null | grep -q ":$PORT "; do
    PORT=$((PORT + 1))
    [ "$PORT" -ge 52000 ] && {
        echo "skip: no free UDP test port"
        exit 0
    }
done
PRIV="$(wg genkey)"
PEER="$(wg genkey | wg pubkey)"
cat >"$R/wg.conf" <<CONF
[Interface]
PrivateKey = $PRIV
Address = 10.89.0.1/24
ListenPort = $PORT
[Peer]
PublicKey = $PEER
AllowedIPs = 10.89.0.2/32
CONF

"$UVROOT" -v 3 -r / --net \
    --net-if=vtun,addr=10.89.0.1/24,up \
    --wg=vtun:conf="$R/wg.conf" \
    /bin/sleep 30 >"$R/run.log" 2>&1 &
PID=$!
sleep 3

echo "--- netvirt output ---"
grep -E 'WireGuard|netvirt: vtun|netvirt = .*vtun' "$R/run.log" | tail -10

ok "conf= file is parsed as a WireGuard config" \
    "$(grep -c 'no PrivateKey' "$R/run.log")" "0"
ok "built-in engine starts" \
    "$(grep -c 'WireGuard engine started' "$R/run.log")" "1"
ok "configured UDP listener is open" \
    "$(ss -lun 2>/dev/null | grep -c ":$PORT ")" "1"
ok "vtun has the configured address" \
    "$(grep -c '10.89.0.1/24' "$R/run.log")" "1"

echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
