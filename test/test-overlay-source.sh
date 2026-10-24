#!/bin/sh
# Source-adapter tests: type 1 and/or type 2 may be a raw/QCOW2/NBD
# image instead of a host directory.
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
    command -v "$tool" >/dev/null 2>&1 || {
        echo "skip: $tool is not installed"
        exit 0
    }
done

make_ext4() {
    local path="$1"
    local seed="$2"
    local size="${3:-16}"

    dd if=/dev/zero of="$path" bs=1M count="$size" status=none
    mke2fs -q -t ext4 -F -d "$seed" "$path"
}

to_raw() {
    local image="$1"
    local raw="$2"

    qemu-img convert -f qcow2 -O raw "$image" "$raw"
}

mkdir -p "$R/seed-lower" "$R/seed-upper" "$R/lower" "$R/upper"
printf 'lower-image\n' > "$R/seed-lower/l"
printf 'upper-image\n' > "$R/seed-upper/u"
printf 'lower-dir\n' > "$R/lower/d"
printf 'lower-dir\n' > "$R/lower/l"
printf 'upper-dir\n' > "$R/upper/o"

# ------------------------------------------------------------------
# type 2 = QCOW2 image, type 1 = directory
# ------------------------------------------------------------------
echo "== type 2 QCOW2, type 1 directory =="
make_ext4 "$R/low.raw" "$R/seed-lower"
qemu-img convert -f raw -O qcow2 "$R/low.raw" "$R/low.qcow2"
"$UVROOT" -r / \
    "--overlay=block,/mnt/ovl,$R/upper,qcow2://$R/low.qcow2" \
    /bin/sh -c '
        printf "READ:"; cat /mnt/ovl/l
        printf "changed\n" > /mnt/ovl/l
        printf "new\n" > /mnt/ovl/n
        printf "READBACK:"; cat /mnt/ovl/l
        /dev/bkc /mnt/ovl/l
        echo bkc:$?
        /dev/bme /mnt/ovl/l
        echo bme:$?
        printf "MERGED:"; cat /mnt/ovl/l
    ' >"$R/qcow2-lower.log" 2>&1
status=$?
echo "--- type 2 QCOW2 output ---"
cat "$R/qcow2-lower.log"
ok "type 2 qcow2 run succeeds" "$status" "0"
ok "type 2 qcow2 is readable" \
    "$(grep -c '^READ:lower-image$' "$R/qcow2-lower.log")" "1"
ok "type 2 qcow2 write is readable back" \
    "$(grep -c '^READBACK:changed$' "$R/qcow2-lower.log")" "1"
ok "type 2 qcow2 bkc reports unmerged" \
    "$(grep -c '^bkc:1$' "$R/qcow2-lower.log")" "1"
ok "type 2 qcow2 bme succeeds" \
    "$(grep -c '^bme:0$' "$R/qcow2-lower.log")" "1"
ok "type 2 qcow2 merged readback" \
    "$(grep -c '^MERGED:changed$' "$R/qcow2-lower.log")" "1"
ok "type 2 qcow2 source is untouched" \
    "$(to_raw "$R/low.qcow2" "$R/low.check"; debugfs -R 'cat /l' "$R/low.check" 2>/dev/null)" "lower-image"
ok "type 1 keeps the new file" \
    "$(cat "$R/upper/n" 2>/dev/null)" "new"

# ------------------------------------------------------------------
# type 2 = raw image, type 1 = directory, bind mode
# ------------------------------------------------------------------
echo "== type 2 raw, type 1 directory =="
make_ext4 "$R/low.raw" "$R/seed-lower"
rm -rf "$R/upper2"
mkdir "$R/upper2"
"$UVROOT" -r / \
    "--overlay=bind,/mnt/ovl,$R/upper2,img://$R/low.raw" \
    /bin/sh -c 'cat /mnt/ovl/l; echo rawchanged > /mnt/ovl/l; cat /mnt/ovl/l' \
    >"$R/raw-lower.log" 2>&1
status=$?
echo "--- type 2 raw output ---"
cat "$R/raw-lower.log"
ok "type 2 raw run succeeds" "$status" "0"
ok "type 2 raw write is readable back" \
    "$(grep -c '^rawchanged$' "$R/raw-lower.log")" "1"
ok "type 2 raw source is untouched" \
    "$(debugfs -R 'cat /l' "$R/low.raw" 2>/dev/null)" "lower-image"

# ------------------------------------------------------------------
# type 2 = NBD image, type 1 = directory
# ------------------------------------------------------------------
echo "== type 2 NBD, type 1 directory =="
make_ext4 "$R/nbd.raw" "$R/seed-lower"
qemu-nbd --format=raw --socket="$R/nbd.sock" --export-name=disk \
    --persistent "$R/nbd.raw" >"$R/nbd.log" 2>&1 &
NBD_PID=$!
for _ in $(seq 1 100); do
    [ -S "$R/nbd.sock" ] && break
    sleep 0.1
done
[ -S "$R/nbd.sock" ] || exit 1
rm -rf "$R/upper3"
mkdir "$R/upper3"
"$UVROOT" -r / \
    "--overlay=block,/mnt/ovl,$R/upper3,nbd+unix:///disk?socket=$R/nbd.sock" \
    /bin/sh -c '
        cat /mnt/ovl/l
        echo nbdchanged > /mnt/ovl/l
        /dev/bkc /mnt/ovl/l
        echo bkc:$?
        /dev/bme /mnt/ovl/l
        echo bme:$?
        cat /mnt/ovl/l
    ' >"$R/nbd-lower.log" 2>&1
status=$?
echo "--- type 2 NBD output ---"
cat "$R/nbd-lower.log"
ok "type 2 NBD run succeeds" "$status" "0"
ok "type 2 NBD bkc/bme work" \
    "$(grep -c '^bme:0$' "$R/nbd-lower.log")" "1"
ok "type 2 NBD source is untouched" \
    "$(debugfs -R 'cat /l' "$R/nbd.raw" 2>/dev/null)" "lower-image"
kill "$NBD_PID" 2>/dev/null || true
wait "$NBD_PID" 2>/dev/null || true
NBD_PID=""

# ------------------------------------------------------------------
# type 1 = QCOW2 image, type 2 = directory
# ------------------------------------------------------------------
echo "== type 1 QCOW2, type 2 directory =="
make_ext4 "$R/up.raw" "$R/seed-upper"
qemu-img convert -f raw -O qcow2 "$R/up.raw" "$R/up.qcow2"
"$UVROOT" -r / \
    "--overlay=block,/mnt/ovl,qcow2://$R/up.qcow2,$R/lower" \
    /bin/sh -c '
        cat /mnt/ovl/l
        cat /mnt/ovl/u
        echo upperwrite > /mnt/ovl/w
        cat /mnt/ovl/w
        echo changed > /mnt/ovl/l
        cat /mnt/ovl/l
    ' >"$R/qcow2-upper.log" 2>&1
status=$?
echo "--- type 1 QCOW2 output ---"
cat "$R/qcow2-upper.log"
ok "type 1 qcow2 run succeeds" "$status" "0"
ok "type 1 qcow2 keeps its initial file" \
    "$(grep -c '^upper-image$' "$R/qcow2-upper.log")" "1"
ok "type 1 qcow2 write is readable back" \
    "$(grep -c '^upperwrite$' "$R/qcow2-upper.log")" "1"
ok "type 1 qcow2 lower change is readable back" \
    "$(grep -c '^changed$' "$R/qcow2-upper.log")" "1"
to_raw "$R/up.qcow2" "$R/up.check"
ok "type 1 qcow2 source received the write" \
    "$(debugfs -R 'cat /w' "$R/up.check" 2>/dev/null)" "upperwrite"
ok "type 1 qcow2 source has .overlay" \
    "$(debugfs -R 'cat /.overlay' "$R/up.check" 2>/dev/null | grep -c '^upper_source 1$')" "1"

# bme is intentionally refused on a source-backed type 1.
"$UVROOT" -r / \
    "--overlay=block,/mnt/ovl,qcow2://$R/up.qcow2,$R/lower" \
    /dev/bme /mnt/ovl/l >"$R/source-bme.log" 2>&1
bme_status=$?
ok "bme refuses a source-backed type 1" "$bme_status" "1"
ok "bme explains the refusal" \
    "$(grep -c 'type 1 is a source' "$R/source-bme.log")" "1"

# A fresh mount must see the data that was synced to the image.
"$UVROOT" -r / \
    "--overlay=block,/mnt/ovl,qcow2://$R/up.qcow2,$R/lower" \
    /bin/cat /mnt/ovl/l >"$R/up-fresh.log" 2>&1
ok "type 1 qcow2 persists across mounts" \
    "$(grep -c '^changed$' "$R/up-fresh.log")" "1"

# ------------------------------------------------------------------
# type 1 = raw image, type 2 = directory
# ------------------------------------------------------------------
echo "== type 1 raw, type 2 directory =="
make_ext4 "$R/up-raw.img" "$R/seed-upper"
"$UVROOT" -r / \
    "--overlay=bind,/mnt/ovl,img://$R/up-raw.img,$R/lower" \
    /bin/sh -c 'cat /mnt/ovl/l; echo rawtyped > /mnt/ovl/r; cat /mnt/ovl/r' \
    >"$R/raw-upper.log" 2>&1
status=$?
echo "--- type 1 raw output ---"
cat "$R/raw-upper.log"
ok "type 1 raw run succeeds" "$status" "0"
ok "type 1 raw source is writable" \
    "$(debugfs -R 'cat /r' "$R/up-raw.img" 2>/dev/null)" "rawtyped"

# ------------------------------------------------------------------
# type 1 = NBD image, type 2 = directory
# ------------------------------------------------------------------
echo "== type 1 NBD, type 2 directory =="
make_ext4 "$R/up-nbd.img" "$R/seed-upper"
qemu-nbd --format=raw --socket="$R/up-nbd.sock" --export-name=disk \
    --persistent "$R/up-nbd.img" >"$R/up-nbd.log" 2>&1 &
NBD_PID=$!
for _ in $(seq 1 100); do
    [ -S "$R/up-nbd.sock" ] && break
    sleep 0.1
done
[ -S "$R/up-nbd.sock" ] || exit 1
"$UVROOT" -r / \
    "--overlay=bind,/mnt/ovl,nbd+unix:///disk?socket=$R/up-nbd.sock,$R/lower" \
    /bin/sh -c 'cat /mnt/ovl/l; echo nbdtyped > /mnt/ovl/n; cat /mnt/ovl/n' \
    >"$R/nbd-upper.log" 2>&1
status=$?
echo "--- type 1 NBD output ---"
cat "$R/nbd-upper.log"
ok "type 1 NBD run succeeds" "$status" "0"
ok "type 1 NBD source is writable" \
    "$(debugfs -R 'cat /n' "$R/up-nbd.img" 2>/dev/null)" "nbdtyped"
kill "$NBD_PID" 2>/dev/null || true
wait "$NBD_PID" 2>/dev/null || true
NBD_PID=""

# ------------------------------------------------------------------
# both types are QCOW2 images
# ------------------------------------------------------------------
echo "== both types QCOW2 =="
make_ext4 "$R/low2.raw" "$R/seed-lower"
qemu-img convert -f raw -O qcow2 "$R/low2.raw" "$R/low2.qcow2"
make_ext4 "$R/up2.raw" "$R/seed-upper"
qemu-img convert -f raw -O qcow2 "$R/up2.raw" "$R/up2.qcow2"
"$UVROOT" -r / \
    "--overlay=block,/mnt/ovl,qcow2://$R/up2.qcow2,qcow2://$R/low2.qcow2" \
    /bin/sh -c 'cat /mnt/ovl/l; cat /mnt/ovl/u; echo both > /mnt/ovl/both; cat /mnt/ovl/both' \
    >"$R/both.log" 2>&1
status=$?
echo "--- both QCOW2 output ---"
cat "$R/both.log"
ok "both-qcow2 run succeeds" "$status" "0"
ok "both-qcow2 lower is readable" \
    "$(grep -c '^lower-image$' "$R/both.log")" "1"
ok "both-qcow2 upper is readable" \
    "$(grep -c '^upper-image$' "$R/both.log")" "1"
ok "both-qcow2 write is readable back" \
    "$(grep -c '^both$' "$R/both.log")" "1"
to_raw "$R/up2.qcow2" "$R/up2.check"
ok "both-qcow2 upper image received the write" \
    "$(debugfs -R 'cat /both' "$R/up2.check" 2>/dev/null)" "both"
to_raw "$R/low2.qcow2" "$R/low2.check"
ok "both-qcow2 lower image is untouched" \
    "$(debugfs -R 'cat /l' "$R/low2.check" 2>/dev/null)" "lower-image"

echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
