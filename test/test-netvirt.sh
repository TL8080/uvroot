#!/bin/sh
#
# The user-mode virtual network: the container is given two mapped
# devices, veth0 (ordinary I/O) and vtun (tailscale/intranet), bridged
# over WireGuard, and `ip` must be able to drive them while the host
# interfaces stay invisible.
#
# The test runs on the host rootfs ("-r /") so that the host ip(8) and
# glibc are reused; it is skipped when any of them is missing.

if [ ! -x "${UVROOT}" ] || [ ! -x /usr/bin/ip ] || [ ! -x /bin/sh ]; then
    exit 125
fi

if ! command -v gcc >/dev/null 2>&1; then
    exit 125
fi

here=$(dirname "$0")
probe=$(mktemp /tmp/netvirt-probe.XXXXXX) || exit 125
trap 'rm -f "${probe}"' EXIT

gcc -O1 -o "${probe}" "${here}/netvirt-probe.c" || exit 125

ip_run() {
    ${UVROOT} -r / --net /usr/bin/ip "$@"
}

# The two mapped devices exist, with their documented names.
ip_run -o link show | grep -q 'veth0'
ip_run -o link show | grep -q 'vtun'

# veth0 carries the ordinary address and the default route; vtun carries
# the tailscale/intranet range.
ip_run -o addr show | grep -q 'inet 10.177.0.2/24'
ip_run -o addr show | grep -q 'inet 100.64.0.2/32'
ip_run route show | grep -q 'default via 10.177.0.1 dev veth0'
ip_run route show | grep -q '100.64.0.0/10 dev vtun'

# Exactly the loopback and the two mapped devices are visible: no host
# interface leaks into the container.
ip_run -o link show | grep -q '^1: lo:'
ip_run -o link show | grep -q '^2: veth0:'
ip_run -o link show | grep -q '^3: vtun:'
test "$(ip_run -o link show | wc -l)" -eq 3

# The container may reconfigure its own devices.
${UVROOT} -r / --net /bin/sh -c '
    set -e
    /usr/bin/ip link set veth0 down
    /usr/bin/ip -o link show veth0 | grep -q "<BROADCAST,MULTICAST>"
    /usr/bin/ip link set veth0 up
    /usr/bin/ip -o link show veth0 | grep -q ",UP>"

    /usr/bin/ip addr add 10.9.9.2/24 dev veth0
    /usr/bin/ip -o addr show veth0 | grep -q "10.9.9.2/24"
    /usr/bin/ip route show | grep -q "10.9.9.0/24 dev veth0"
    /usr/bin/ip addr del 10.9.9.2/24 dev veth0
    /usr/bin/ip -o addr show veth0 | grep -q "10.177.0.2/24"
    if /usr/bin/ip -o addr show veth0 | grep -q "10.9.9.2"; then exit 1; fi

    /usr/bin/ip route add 192.168.5.0/24 dev vtun
    /usr/bin/ip route show | grep -q "192.168.5.0/24 dev vtun"
    /usr/bin/ip route del 192.168.5.0/24 dev vtun

    /usr/bin/ip link add name dummy0 type dummy
    /usr/bin/ip -o link show dummy0 | grep -q "dummy0"
    /usr/bin/ip link del dummy0
    if /usr/bin/ip -o link show dummy0 >/dev/null 2>&1; then exit 1; fi
'

# getifaddrs(3), the legacy ioctls and a raw rtnetlink dump all agree.
${UVROOT} -r / --net "${probe}" | grep -q 'NETVIRT-PROBE OK'

# Sanity check: without --net there is no virtual device, so the
# isolation above really comes from the option.
if ${UVROOT} -r / /usr/bin/ip -o link show | grep -q 'veth0'; then
    echo "veth0 is visible without --net" >&2
    exit 1
fi
