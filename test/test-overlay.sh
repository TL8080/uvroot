#!/bin/sh
# Overlay regression checks: bind mode, block mode, bkc/bme, and the
# database/whiteout rules.  The block tests use the static overlayctl
# helper built next to uvroot; they are skipped when it is absent.
set -u

UVROOT="${UVROOT:-$(cd "$(dirname "$0")/.." && pwd)/src/uvroot}"
HELPER="$(dirname "$UVROOT")/overlayctl"
R=/tmp/overlay-test
R2=/tmp/overlay-mixed-mode
pass=0
fail=0

ok() {
    if [ "$2" = "$3" ]; then
        echo "  ok   $1"
        pass=$((pass + 1))
    else
        echo "  FAIL $1 (got [$2] want [$3])"
        fail=$((fail + 1))
    fi
}

ov() {
    mode="$1"; shift
    "$UVROOT" -r / "--overlay=$mode,/mnt/ovl,$R/upper,$R/lower" "$@"
}

ov2() {
    mode="$1"; shift
    "$UVROOT" -r / "--overlay=$mode,/mnt/ovl,$R2/upper,$R2/lower" "$@"
}

rm -rf "$R" "$R2"
mkdir -p "$R/upper" "$R/lower/dir"
printf 'lower-a\n' > "$R/lower/a"
printf 'lower-b\n' > "$R/lower/dir/b"

echo "== bind mode: read, copy-up, whiteout =="
ok "lower file is visible" "$(ov bind /bin/cat /mnt/ovl/a 2>/dev/null)" "lower-a"
ok "nested lower file is visible" "$(ov bind /bin/cat /mnt/ovl/dir/b 2>/dev/null)" "lower-b"
ov bind /bin/sh -c "printf 'upper-new\\n' > /mnt/ovl/new" >/dev/null 2>&1
ok "new file stays in type 1" "$(cat "$R/upper/new")" "upper-new"
ov bind /bin/sh -c "printf 'changed\\n' > /mnt/ovl/a" >/dev/null 2>&1
ok "modified file is copied up" "$(cat "$R/upper/a")" "changed"
ok "lower copy is untouched" "$(cat "$R/lower/a")" "lower-a"
ok "upper copy wins" "$(ov bind /bin/cat /mnt/ovl/a 2>/dev/null)" "changed"
ov bind /bin/rm /mnt/ovl/a >/dev/null 2>&1
ok "deleting lower file records a whiteout" \
    "$(grep -c '	a$' "$R/upper/.overlay")" "1"
ok "deleted lower file is hidden" \
    "$(ov bind /bin/sh -c 'test -e /mnt/ovl/a && echo yes || echo no')" "no"
ok "database is hidden from the merged view" \
    "$(ov bind /bin/sh -c 'test -e /mnt/ovl/.overlay && echo yes || echo no')" "no"

echo "== bind mode: symlinks and hard links =="
ov bind /bin/sh -c 'printf "src\n" > /mnt/ovl/src' >/dev/null 2>&1
ov bind /bin/ln -s src /mnt/ovl/link >/dev/null 2>&1
ov bind /bin/ln /mnt/ovl/src /mnt/ovl/hard >/dev/null 2>&1
ok "a symlink is copied into type 1" \
    "$(ov bind /bin/readlink /mnt/ovl/link 2>/dev/null)" "src"
ok "both hard-link names expose nlink 2" \
    "$(ov bind /bin/sh -c 'stat -c %h /mnt/ovl/src /mnt/ovl/hard' 2>/dev/null | sort -u)" "2"

echo "== bind mode: directory rename =="
ov bind /bin/mv /mnt/ovl/dir /mnt/ovl/dir2 >/dev/null 2>&1
ok "renamed directory is in type 1" \
    "$(ov bind /bin/cat /mnt/ovl/dir2/b 2>/dev/null)" "lower-b"
ok "old directory is hidden" \
    "$(ov bind /bin/sh -c 'test -e /mnt/ovl/dir && echo yes || echo no')" "no"

echo "== bind mode: recursive removal =="
mkdir -p "$R/lower/del/sub"
printf 'x\n' > "$R/lower/del/sub/x"
ov bind /bin/rm -rf /mnt/ovl/del >/dev/null 2>&1
ok "rm -rf records the directory whiteout" \
    "$(grep -c '^D	del$' "$R/upper/.overlay")" "1"
ok "rm -rf stays gone in a fresh run" \
    "$(ov bind /bin/sh -c 'test -e /mnt/ovl/del && echo yes || echo no')" "no"

echo "== block mode: sparse delta =="
# Restore a clean pair of trees for the block tests.
rm -rf "$R/upper" "$R/lower"
mkdir -p "$R/upper" "$R/lower/dir"
dd if=/dev/urandom of="$R/lower/data" bs=1K count=16 status=none
dd if=/dev/urandom of="$R/lower/dir/one" bs=1K count=8 status=none
dd if=/dev/urandom of="$R/lower/dir/two" bs=1K count=8 status=none
cp "$R/lower/data" "$R/orig"
cp "$R/lower/dir/one" "$R/orig.one"
cp "$R/lower/dir/two" "$R/orig.two"

ov block /bin/sh -c \
    "dd if=/dev/zero of=/mnt/ovl/data bs=1 seek=4096 count=4096 conv=notrunc status=none" \
    >/dev/null 2>&1
ok "block database records one file" \
    "$(grep -c '^B	data	' "$R/upper/.overlay")" "1"
ok "type 1 is sparse" \
    "$([ "$(du -k "$R/upper/data" | cut -f1)" -lt 16 ] && echo sparse || echo full)" "sparse"
cp "$R/orig" "$R/expected"
dd if=/dev/zero of="$R/expected" bs=1 seek=4096 count=4096 conv=notrunc status=none
ov block /bin/cat /mnt/ovl/data > "$R/merged" 2>/dev/null
ok "fresh read merges lower and delta" \
    "$(cmp -s "$R/merged" "$R/expected" && echo yes || echo no)" "yes"

if [ -x "$HELPER" ]; then
    echo "== bkc / bme =="
    bkc_status=0
    ov block /dev/bkc /mnt/ovl/data > "$R/bkc.out" 2>&1 || bkc_status=$?
    ok "bkc reports unmerged" "$(grep -c 'unmerged hash-ok' "$R/bkc.out")" "1"
    ok "bkc exit status is 1" "$bkc_status" "1"
    ov block /dev/bme -h /mnt/ovl/data > "$R/bme.out" 2>&1
    ok "bme merges" "$(grep -c '^merged data' "$R/bme.out")" "1"
    ok "bme leaves a full upper file" \
        "$(cmp -s "$R/upper/data" "$R/merged" && echo yes)" "yes"
    ok "bme removes the block entry" \
        "$(grep -c '^B	data	' "$R/upper/.overlay" || true)" "0"
    ov block /dev/bkc /mnt/ovl/data > "$R/bkc2.out" 2>&1
    ok "bkc sees merged data" "$(grep -c '^data merged' "$R/bkc2.out")" "1"

    echo "== bme -r =="
    ov block /bin/sh -c \
        "dd if=/dev/zero of=/mnt/ovl/dir/one bs=1 seek=0 count=4096 conv=notrunc status=none; \
         dd if=/dev/zero of=/mnt/ovl/dir/two bs=1 seek=0 count=4096 conv=notrunc status=none" \
        >/dev/null 2>&1
    ov block /dev/bme -r /mnt/ovl/dir >/dev/null 2>&1
    ok "bme -r removes every block entry" \
        "$(grep -c '^B	' "$R/upper/.overlay" || true)" "0"

    echo "== bme -h detects a changed lower file =="
    # Recreate a delta, then change the lower copy behind the overlay.
    ov block /bin/sh -c \
        "dd if=/dev/zero of=/mnt/ovl/data bs=1 seek=0 count=4096 conv=notrunc status=none" \
        >/dev/null 2>&1
    printf 'X' | dd of="$R/lower/data" bs=1 seek=8192 conv=notrunc status=none
    bme_status=0
    ov block /dev/bme -h /mnt/ovl/data >/dev/null 2>&1 || bme_status=$?
    ok "hash mismatch aborts bme -h" "$bme_status" "1"

    echo "== bme respects --read-only =="
    ro_status=0
    "$UVROOT" -r / --read-only \
        "--overlay=block,/mnt/ovl,$R/upper,$R/lower" \
        /dev/bme /mnt/ovl/data >/dev/null 2>&1 || ro_status=$?
    ok "bme refuses a read-only overlay" "$ro_status" "1"
    ok "the read-only state is recorded" \
        "$(grep -c '^read_only 1$' "$R/upper/.overlay")" "1"
else
    echo "== bkc / bme skipped (no overlayctl) =="
fi

echo "== mode is a write policy: mixed upper formats =="
mkdir -p "$R2/upper" "$R2/lower"
dd if=/dev/urandom of="$R2/lower/data" bs=1K count=16 status=none
cp "$R2/lower/data" "$R2/lower-orig"
cp "$R2/lower/data" "$R2/expected"
ov2 block /bin/sh -c \
    "dd if=/dev/zero of=/mnt/ovl/data bs=1 seek=4096 count=4096 conv=notrunc status=none" \
    >/dev/null 2>&1
dd if=/dev/zero of="$R2/expected" bs=1 seek=4096 count=4096 conv=notrunc status=none
ok "a block-created upper is readable with bind" \
    "$(ov2 bind /bin/cat /mnt/ovl/data 2>/dev/null | cmp -s - "$R2/expected" && echo yes || echo no)" "yes"
ok "the block entry survived the bind mount" \
    "$(grep -c '^B	data	' "$R2/upper/.overlay" || true)" "1"
ok "the selected bind mode is recorded" \
    "$(grep -c '^mode bind$' "$R2/upper/.overlay")" "1"
printf 'bind-write\n' > "$R2/newcontent"
ov2 bind /bin/sh -c "cat '$R2/newcontent' > /mnt/ovl/data" >/dev/null 2>&1
ok "a bind write replaces the block entry with a full file" \
    "$(cat "$R2/upper/data")" "bind-write"
ok "the block entry is gone" \
    "$(grep -c '^B	data	' "$R2/upper/.overlay" || true)" "0"
ok "the lower copy is untouched" \
    "$(cmp -s "$R2/lower/data" "$R2/lower-orig" && echo yes || echo no)" "yes"
ok "the full file is readable in block mode too" \
    "$(ov2 block /bin/cat /mnt/ovl/data 2>/dev/null)" "bind-write"

echo "== --overlay-root combines with one ordinary overlay =="
R3=/tmp/overlay-test/root-combined
rm -rf "$R3"
mkdir -p "$R3/root" "$R3/lower" "$R3/be-upper"
printf 'root-upper\n' > "$R3/root/root-file"
printf 'lower-only\n' > "$R3/lower/lower-file"
printf 'vperm-secret\n' > "$R3/root/.uvroot-vperm"
ro() {
    "$UVROOT" -r "$R3/lower"         -b /bin:/bin -b /usr:/usr -b /lib:/lib -b /lib64:/lib64         "--overlay-root=block,$R3/root"         "--overlay=block,/be,$R3/be-upper,$R3/lower" "$@"
}
ok "root overlay reads its upper" "$(ro /bin/cat /root-file 2>/dev/null)" "root-upper"
ok "root overlay reads its lower" "$(ro /bin/cat /lower-file 2>/dev/null)" "lower-only"
ok "root overlay falls back from lower to upper" \
    "$(ro /bin/sh -c 'printf upper-write > /new-root; cat /new-root' 2>/dev/null)" "upper-write"
ok "the ordinary overlay still reads its lower" \
    "$(ro /bin/cat /be/lower-file 2>/dev/null)" "lower-only"
ok "the ordinary overlay writes its own upper" \
    "$(ro /bin/sh -c 'printf be-write > /be/new; cat /be/new' 2>/dev/null)" "be-write"
ok "an upper .uvroot-vperm is hidden" \
    "$(ro /bin/ls -a / 2>/dev/null | grep -c '^\.uvroot-vperm$')" "0"
ok "an upper .uvroot-vperm is unreadable" \
    "$(ro /bin/cat /.uvroot-vperm 2>/dev/null)" ""
ok "root and ordinary uppers stay separate" \
    "$(test -f "$R3/root/new-root" && test ! -e "$R3/be-upper/new-root" && test -f "$R3/be-upper/new" && echo yes || echo no)" "yes"

echo "== multiple ordinary overlay mounts =="
R4=/tmp/overlay-test/multi
rm -rf "$R4"
mkdir -p "$R4/root" "$R4/u1" "$R4/l1" "$R4/u2" "$R4/l2"
printf 'one\n' > "$R4/l1/one"
printf 'two\n' > "$R4/l2/two"
multi() {
    "$UVROOT" -r "$R4/root" -b /bin:/bin -b /usr:/usr -b /lib:/lib \
        -b /lib64:/lib64 \
        "--overlay=block,/o1,$R4/u1,$R4/l1" \
        "--overlay=block,/o2,$R4/u2,$R4/l2" "$@"
}
ok "first overlay reads its lower" "$(multi /bin/cat /o1/one 2>/dev/null)" "one"
ok "second overlay reads its lower" "$(multi /bin/cat /o2/two 2>/dev/null)" "two"
ok "both overlays write independently" \
    "$(multi /bin/sh -c 'printf x >/o1/x; printf y >/o2/y; cat /o1/x /o2/y' 2>/dev/null)" "xy"
ok "second overlay has its own hidden paths" \
    "$(test -f "$R4/u2/y" && test ! -e "$R4/u1/y" && echo yes || echo no)" "yes"

echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
