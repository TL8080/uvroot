#!/bin/sh
# Overlay regression checks: bind mode, block mode, bkc/bme, and the
# database/whiteout rules.  The block tests use the static overlayctl
# helper built next to uvroot; they are skipped when it is absent.
set -u

UVROOT="${UVROOT:-$(cd "$(dirname "$0")/.." && pwd)/src/uvroot}"
HELPER="$(dirname "$UVROOT")/overlayctl"
R=/tmp/overlay-test
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

rm -rf "$R"
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

echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
