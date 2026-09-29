#!/bin/sh
# End-to-end verification of the netfs NFS backend.
#
# An NFS export is a directory tree, so the backend is a directory
# transport like FTP and SMB: listing, reading, writing, mkdir, rmdir,
# unlink, rename and chmod are forwarded to the server, and symbolic
# links found there are followed through the local mirror.
#
# NFS servers normally need root (the kernel server) or the portmapper
# on port 111, so this script drives a user-space server instead:
#
#     UNFSD=/path/to/unfsd UVROOT=/path/to/uvroot sh test/test-nfs.sh
#
# The server is started on high ports without the portmapper.  When
# UNFSD is not set the suite is skipped, which keeps `make -C test`
# usable on machines without one.
#
# Alternatively, point the suite at an already running server with
# NFS_TEST_URI (the nfs:// URI, query arguments included) and
# NFS_TEST_DIR (the host directory it exports).

set -u

UVROOT="${UVROOT:-$(cd "$(dirname "$0")/.." && pwd)/src/uvroot}"
WORK="$(mktemp -d)"
UNFSD_PID=""

NFS_PORT="${NFS_PORT:-14049}"
MOUNT_PORT="${MOUNT_PORT:-14048}"
EXPORT_DIR="${NFS_TEST_DIR:-$WORK/export}"
NFS_URI="${NFS_TEST_URI:-}"

pass=0
fail=0

cleanup() {
    if [ -n "$UNFSD_PID" ]; then
        kill "$UNFSD_PID" 2>/dev/null || true
        wait "$UNFSD_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

check() {
    label="$1"
    shift
    if "$@"; then
        echo "  ok   $label"
        pass=$((pass + 1))
    else
        echo "  FAIL $label"
        fail=$((fail + 1))
    fi
}

wait_port() {
    port="$1"
    i=0
    if ! command -v python3 >/dev/null 2>&1; then
        sleep 1
        return 0
    fi
    while [ "$i" -lt 100 ]; do
        if python3 -c "import socket,sys
s = socket.socket()
s.settimeout(0.2)
sys.exit(0 if s.connect_ex(('127.0.0.1', $port)) == 0 else 1)" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

# ------------------------------------------------------------------
# Server setup
# ------------------------------------------------------------------

if [ -z "$NFS_URI" ]; then
    if [ -z "${UNFSD:-}" ] || [ ! -x "$UNFSD" ]; then
        echo "== nfs (skipped, set UNFSD to a user-space unfsd binary) =="
        echo
        echo "passed: 0   failed: 0"
        exit 0
    fi

    rm -rf "$EXPORT_DIR"
    mkdir -p "$EXPORT_DIR/sub" "$EXPORT_DIR/emptydir"
    printf 'hello from nfs\n' >"$EXPORT_DIR/hello.txt"
    printf 'nested file\n' >"$EXPORT_DIR/sub/nested.txt"
    printf '0123456789' >"$EXPORT_DIR/ten.bin"
    ln -sf hello.txt "$EXPORT_DIR/hostlink"

    printf '%s (rw,no_root_squash,insecure)\n' "$EXPORT_DIR" >"$WORK/exports"
    "$UNFSD" -d -p -t -n "$NFS_PORT" -m "$MOUNT_PORT" \
        -e "$WORK/exports" -i "$WORK/unfsd.pid" >"$WORK/unfsd.log" 2>&1 &
    UNFSD_PID=$!

    if ! wait_port "$NFS_PORT"; then
        echo "== nfs (the user-space server did not start) =="
        cat "$WORK/unfsd.log" >&2
        echo
        echo "passed: 0   failed: 1"
        exit 1
    fi

    NFS_URI="nfs://127.0.0.1$EXPORT_DIR?version=3&nfsport=$NFS_PORT&mountport=$MOUNT_PORT"
fi

if [ ! -d "$EXPORT_DIR" ]; then
    echo "NFS_TEST_DIR ($EXPORT_DIR) is not a directory" >&2
    exit 1
fi

# ------------------------------------------------------------------
# Checks
# ------------------------------------------------------------------

echo "== nfs =="

check "advertised option" sh -c \
    "'$UVROOT' --help 2>&1 | grep -q -- '--nfs'"

check "list" sh -c \
    "'$UVROOT' -r / \"--nfs=/mnt/nfs:$NFS_URI\" /bin/ls /mnt/nfs 2>/dev/null | grep -q hello.txt"

check "read file" sh -c \
    "[ \"\$('$UVROOT' -r / \"--nfs=/mnt/nfs:$NFS_URI\" /bin/cat /mnt/nfs/hello.txt 2>/dev/null)\" = 'hello from nfs' ]"

check "read nested" sh -c \
    "[ \"\$('$UVROOT' -r / \"--nfs=/mnt/nfs:$NFS_URI\" /bin/cat /mnt/nfs/sub/nested.txt 2>/dev/null)\" = 'nested file' ]"

check "stat size" sh -c \
    "[ \"\$('$UVROOT' -r / \"--nfs=/mnt/nfs:$NFS_URI\" /bin/stat -c %s /mnt/nfs/ten.bin 2>/dev/null)\" = 10 ]"

check "binary round trip" sh -c \
    "'$UVROOT' -r / \"--nfs=/mnt/nfs:$NFS_URI\" /bin/sh -c 'cat /mnt/nfs/ten.bin > /mnt/nfs/copy.bin' 2>/dev/null &&
     [ \"\$('$UVROOT' -r / \"--nfs=/mnt/nfs:$NFS_URI\" /bin/cat /mnt/nfs/copy.bin 2>/dev/null)\" = 0123456789 ]"

check "create file reaches the server" sh -c \
    "'$UVROOT' -r / \"--nfs=/mnt/nfs:$NFS_URI\" /bin/sh -c 'echo written > /mnt/nfs/new.txt' 2>/dev/null &&
     [ \"\$(cat '$EXPORT_DIR/new.txt')\" = written ]"

check "mkdir reaches the server" sh -c \
    "'$UVROOT' -r / \"--nfs=/mnt/nfs:$NFS_URI\" /bin/mkdir /mnt/nfs/dir 2>/dev/null &&
     [ -d '$EXPORT_DIR/dir' ]"

check "rename" sh -c \
    "'$UVROOT' -r / \"--nfs=/mnt/nfs:$NFS_URI\" /bin/mv /mnt/nfs/new.txt /mnt/nfs/renamed.txt 2>/dev/null &&
     [ -f '$EXPORT_DIR/renamed.txt' ]"

check "unlink" sh -c \
    "'$UVROOT' -r / \"--nfs=/mnt/nfs:$NFS_URI\" /bin/rm /mnt/nfs/renamed.txt 2>/dev/null &&
     [ ! -e '$EXPORT_DIR/renamed.txt' ]"

check "rmdir" sh -c \
    "'$UVROOT' -r / \"--nfs=/mnt/nfs:$NFS_URI\" /bin/rmdir /mnt/nfs/dir 2>/dev/null &&
     [ ! -e '$EXPORT_DIR/dir' ]"

check "follows a server-side symlink" sh -c \
    "[ \"\$('$UVROOT' -r / \"--nfs=/mnt/nfs:$NFS_URI\" /bin/readlink /mnt/nfs/hostlink 2>/dev/null)\" = hello.txt ] &&
     [ \"\$('$UVROOT' -r / \"--nfs=/mnt/nfs:$NFS_URI\" /bin/cat /mnt/nfs/hostlink 2>/dev/null)\" = 'hello from nfs' ]"

check "creating a symlink is refused like FTP/SMB" sh -c \
    "! '$UVROOT' -r / \"--nfs=/mnt/nfs:$NFS_URI\" /bin/ln -s x /mnt/nfs/newlink 2>/dev/null"

check "NFS is refused as the guest root" sh -c \
    "'$UVROOT' -r / \"--nfs=/:$NFS_URI\" /bin/true 2>&1 | grep -q 'cannot be used as the guest root'"

echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
