#!/bin/sh
# Mixed-backend test: overlay (host directories) together with a QCOW2
# image and an NBD network image in the same container.  The three
# namespaces must stay independent while all are writable.
set -u

UVROOT="${UVROOT:-$(cd "$(dirname "$0")/.." && pwd)/src/uvroot}"
HELPER="$(dirname "$UVROOT")/overlayctl"
R="$(mktemp -d)"
NBD_PID=""
pass=0
fail=0

cleanup() {
    if [ -n "$NBD_PID" ]; then
        kill "$NBD_PID" 2>/dev/null || true
        wait "$NBD_PID" 2>/dev/null || true
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

for tool in qemu-img qemu-nbd mke2fs debugfs; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "skip: $tool is not installed"
        exit 0
    fi
done

mkdir -p "$R/seed" "$R/lower" "$R/upper" "$R/lower2" "$R/upper2"
printf 'image-hello\n' > "$R/seed/hello.txt"
printf 'overlay-lower\n' > "$R/lower/lower.txt"
printf 'bind-lower\n' > "$R/lower2/bind.txt"

# QCOW2 image with a small ext4 filesystem.
dd if=/dev/zero of="$R/disk.raw" bs=1M count=16 status=none
mke2fs -q -t ext4 -F -d "$R/seed" "$R/disk.raw"
qemu-img convert -f raw -O qcow2 "$R/disk.raw" "$R/disk.qcow2"

# Raw image with the same seed.
dd if=/dev/zero of="$R/raw.img" bs=1M count=16 status=none
mke2fs -q -t ext4 -F -d "$R/seed" "$R/raw.img"

# NBD network image with the same seed.
dd if=/dev/zero of="$R/nbd.raw" bs=1M count=16 status=none
mke2fs -q -t ext4 -F -d "$R/seed" "$R/nbd.raw"
qemu-nbd --format=raw --socket="$R/nbd.sock" --export-name=disk \
    --persistent "$R/nbd.raw" >"$R/nbd.log" 2>&1 &
NBD_PID=$!
for _ in $(seq 1 100); do
    [ -S "$R/nbd.sock" ] && break
    sleep 0.1
done
if [ ! -S "$R/nbd.sock" ]; then
    cat "$R/nbd.log" >&2
    exit 1
fi

"$UVROOT" -r / \
    "--qcow2=/img:$R/disk.qcow2" \
    "--img=/raw:$R/raw.img" \
    "--nbd=/nbd:nbd+unix:///disk?socket=$R/nbd.sock" \
    "--overlay=block,/mnt/ovl,$R/upper,$R/lower" \
    /bin/sh -c '
        printf 'QCOW2:'; cat /img/hello.txt
        printf 'RAW:'; cat /raw/hello.txt
        printf 'NBD:'; cat /nbd/hello.txt
        printf 'OVERLAY:'; cat /mnt/ovl/lower.txt
        printf "image-new\n" > /img/new.txt
        printf "raw-new\n" > /raw/new.txt
        printf "nbd-new\n" > /nbd/new.txt
        printf "overlay-new\n" > /mnt/ovl/new.txt
        printf "overlay-changed\n" > /mnt/ovl/lower.txt
        /dev/bkc /mnt/ovl/lower.txt
        echo bkc:$?
        /dev/bme /mnt/ovl/lower.txt
        echo bme:$?
        printf 'QCOW2READ:'; cat /img/new.txt
        printf 'RAWREAD:'; cat /raw/new.txt
        printf 'NBDREAD:'; cat /nbd/new.txt
        printf 'OVERLAYREAD:'; cat /mnt/ovl/lower.txt
        printf 'OVERLAYNEW:'; cat /mnt/ovl/new.txt
    ' >"$R/run.log" 2>&1
run_status=$?

echo "--- block-mode mixed run (real output) ---"
cat "$R/run.log"
echo "--- end block-mode output ---"

ok "mixed container exits successfully" "$run_status" "0"
ok "QCOW2 image is readable" \
    "$(grep -c '^QCOW2:image-hello$' "$R/run.log")" "1"
ok "raw image is readable" \
    "$(grep -c '^RAW:image-hello$' "$R/run.log")" "1"
ok "NBD image is readable" \
    "$(grep -c '^NBD:image-hello$' "$R/run.log")" "1"
ok "overlay lower is readable" \
    "$(grep -c '^OVERLAY:overlay-lower$' "$R/run.log")" "1"
ok "overlay block check reports unmerged data" \
    "$(grep -c 'bkc:1' "$R/run.log")" "1"
ok "overlay merge succeeds" \
    "$(grep -c 'bme:0' "$R/run.log")" "1"
ok "QCOW2 write is readable back" \
    "$(grep -c '^QCOW2READ:image-new$' "$R/run.log")" "1"
ok "raw write is readable back" \
    "$(grep -c '^RAWREAD:raw-new$' "$R/run.log")" "1"
ok "NBD write is readable back" \
    "$(grep -c '^NBDREAD:nbd-new$' "$R/run.log")" "1"
ok "merged overlay is readable back" \
    "$(grep -c '^OVERLAYREAD:overlay-changed$' "$R/run.log")" "1"
ok "overlay new file is readable back" \
    "$(grep -c '^OVERLAYNEW:overlay-new$' "$R/run.log")" "1"
ok "overlay upper keeps the new file" \
    "$(cat "$R/upper/new.txt" 2>/dev/null)" "overlay-new"
ok "overlay lower is untouched" \
    "$(cat "$R/lower/lower.txt")" "overlay-lower"
ok "overlay upper has the merged file" \
    "$(cat "$R/upper/lower.txt" 2>/dev/null)" "overlay-changed"

# The same image and network mounts, with a bind-mode overlay.
"$UVROOT" -r / \
    "--qcow2=/img:$R/disk.qcow2" \
    "--img=/raw:$R/raw.img" \
    "--nbd=/nbd:nbd+unix:///disk?socket=$R/nbd.sock" \
    "--overlay=bind,/mnt/ovl2,$R/upper2,$R/lower2" \
    /bin/sh -c '
        cat /img/new.txt
        cat /raw/new.txt
        cat /nbd/new.txt
        cat /mnt/ovl2/bind.txt
        printf "bind-new\n" > /mnt/ovl2/new.txt
        printf 'QCOW2READ:'; cat /img/new.txt
        printf 'RAWREAD:'; cat /raw/new.txt
        printf 'NBDREAD:'; cat /nbd/new.txt
        printf 'OVERLAYREAD:'; cat /mnt/ovl2/new.txt
    ' >"$R/bind.log" 2>&1
bind_status=$?

echo "--- bind-mode mixed run (real output) ---"
cat "$R/bind.log"
echo "--- end bind-mode output ---"

ok "bind-mode mixed container exits successfully" "$bind_status" "0"
ok "bind-mode sees all image writes" \
    "$(grep -c '^image-new$' "$R/bind.log")" "1"
ok "bind-mode sees all raw writes" \
    "$(grep -c '^raw-new$' "$R/bind.log")" "1"
ok "bind-mode sees all NBD writes" \
    "$(grep -c '^nbd-new$' "$R/bind.log")" "1"
ok "bind-mode overlay lower is readable" \
    "$(grep -c '^bind-lower$' "$R/bind.log")" "1"
ok "bind-mode QCOW2 write is readable back" \
    "$(grep -c '^QCOW2READ:image-new$' "$R/bind.log")" "1"
ok "bind-mode raw write is readable back" \
    "$(grep -c '^RAWREAD:raw-new$' "$R/bind.log")" "1"
ok "bind-mode NBD write is readable back" \
    "$(grep -c '^NBDREAD:nbd-new$' "$R/bind.log")" "1"
ok "bind-mode overlay write is readable back" \
    "$(grep -c '^OVERLAYREAD:bind-new$' "$R/bind.log")" "1"
ok "bind-mode overlay keeps the new file in type 1" \
    "$(cat "$R/upper2/new.txt" 2>/dev/null)" "bind-new"

# The image writes must be visible on the host side and the QCOW2
# container must remain valid.
qemu-img convert -f qcow2 -O raw "$R/disk.qcow2" "$R/qcheck.raw"
ok "QCOW2 write reached the image" \
    "$(debugfs -R 'cat /new.txt' "$R/qcheck.raw" 2>/dev/null)" "image-new"
ok "raw write reached the image" \
    "$(debugfs -R 'cat /new.txt' "$R/raw.img" 2>/dev/null)" "raw-new"
ok "NBD write reached the image" \
    "$(debugfs -R 'cat /new.txt' "$R/nbd.raw" 2>/dev/null)" "nbd-new"
qemu-img check "$R/disk.qcow2" >/dev/null 2>&1
ok "QCOW2 container is still valid" "$?" "0"

echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
