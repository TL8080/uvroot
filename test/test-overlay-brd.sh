#!/bin/sh
# brd: drop duplicated blocks/files from type 1.  -r walks a directory.
set -u

UVROOT="${UVROOT:-$(cd "$(dirname "$0")/.." && pwd)/src/uvroot}"
R="$(mktemp -d)"
pass=0
fail=0

cleanup() {
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

sha() {
    sha256sum "$1" | cut -d' ' -f1
}

# ------------------------------------------------------------------
# block mode: one redundant block and one changed block
# ------------------------------------------------------------------
echo "== block partial dedupe =="
mkdir -p "$R/b-upper" "$R/b-lower"
dd if=/dev/urandom of="$R/b-lower/data" bs=1024 count=64 status=none
cp "$R/b-lower/data" "$R/b-expected"
dd if=/dev/zero of="$R/b-expected" bs=4096 seek=2 count=1 \
    conv=notrunc status=none
EXPECTED="$(sha "$R/b-expected")"
# block 1 is identical to lower; block 2 is zero, so it stays.
dd if="$R/b-lower/data" of="$R/b-upper/data" bs=4096 skip=1 seek=1 \
    count=1 conv=notrunc status=none
dd if=/dev/zero of="$R/b-upper/data" bs=4096 seek=2 count=1 \
    conv=notrunc status=none
cat > "$R/b-upper/.overlay" <<DB
# uvroot overlay database v1
mode block
guest /be
upper $R/b-upper
lower $R/b-lower
read_only 0
upper_source 0
B	data	65536	1792865000	100644	4096	$EXPECTED	4096:4096,8192:4096
DB

"$UVROOT" -r / "--overlay=block,/be,$R/b-upper,$R/b-lower" \
    /dev/brd /be/data >"$R/brd-block.log" 2>&1
status=$?
echo "--- block brd output ---"
cat "$R/brd-block.log"
echo "--- block database ---"
grep '^B' "$R/b-upper/.overlay" || true

ok "block brd exits successfully" "$status" "0"
ok "block brd reports dedupe" \
    "$(grep -c '^deduped data$' "$R/brd-block.log")" "1"
ok "only the changed block remains" \
    "$(grep -c '8192:4096' "$R/b-upper/.overlay")" "1"
ok "redundant block range is gone" \
    "$(grep -c '4096:4096' "$R/b-upper/.overlay")" "0"
ok "upper file now has one block" \
    "$(stat -c %b "$R/b-upper/data")" "8"
ok "stored hash is unchanged" \
    "$(grep -c "$EXPECTED" "$R/b-upper/.overlay")" "1"

"$UVROOT" -r / "--overlay=block,/be,$R/b-upper,$R/b-lower" \
    /dev/bkc /be/data >"$R/bkc.log" 2>&1
ok "bkc still verifies the merged hash" \
    "$(grep -c 'unmerged hash-ok' "$R/bkc.log")" "1"

# ------------------------------------------------------------------
# block mode: -r removes two whole B entries (exercises entry removal
# while iterating the database)
# ------------------------------------------------------------------
echo "== block recursive dedupe =="
mkdir -p "$R/r-upper" "$R/r-lower"
for name in a b; do
    dd if=/dev/urandom of="$R/r-lower/$name" bs=1024 count=64 status=none
    cp "$R/r-lower/$name" "$R/r-expected-$name"
    H="$(sha "$R/r-expected-$name")"
    dd if="$R/r-lower/$name" of="$R/r-upper/$name" bs=4096 skip=1 seek=1 \
        count=1 conv=notrunc status=none
    printf 'B\t%s\t65536\t1792865000\t100644\t4096\t%s\t4096:4096\n' \
        "$name" "$H" >>"$R/r-upper/.overlay"
done
cat > "$R/r-header" <<DB
# uvroot overlay database v1
mode block
guest /be
upper $R/r-upper
lower $R/r-lower
read_only 0
upper_source 0
DB
cat "$R/r-header" "$R/r-upper/.overlay" >"$R/r-overlay.tmp"
mv "$R/r-overlay.tmp" "$R/r-upper/.overlay"

"$UVROOT" -r / "--overlay=block,/be,$R/r-upper,$R/r-lower" \
    /dev/brd -r /be >"$R/brd-rec.log" 2>&1
status=$?
echo "--- recursive brd output ---"
cat "$R/brd-rec.log"
ok "recursive brd exits successfully" "$status" "0"
ok "recursive brd removed a" \
    "$(grep -c '^deduped a$' "$R/brd-rec.log")" "1"
ok "recursive brd removed b" \
    "$(grep -c '^deduped b$' "$R/brd-rec.log")" "1"
ok "recursive brd left no B entries" \
    "$(grep -c '^B' "$R/r-upper/.overlay" || true)" "0"
ok "recursive brd removed both upper files" \
    "$([ ! -e "$R/r-upper/a" ] && [ ! -e "$R/r-upper/b" ] \
        && echo absent || echo present)" "absent"

# ------------------------------------------------------------------
# bind mode: identical upper file is removed, different one is kept
# ------------------------------------------------------------------
echo "== bind recursive dedupe =="
mkdir -p "$R/f-upper/d" "$R/f-lower/d"
printf 'same\n' > "$R/f-lower/same"
printf 'same\n' > "$R/f-upper/same"
printf 'lower\n' > "$R/f-lower/d/diff"
printf 'upper\n' > "$R/f-upper/d/diff"
printf 'same\n' > "$R/f-lower/d/same"
cp -p "$R/f-lower/same" "$R/f-upper/d/same"
cat > "$R/f-upper/.overlay" <<DB
# uvroot overlay database v1
mode bind
guest /be
upper $R/f-upper
lower $R/f-lower
read_only 0
upper_source 0
DB

"$UVROOT" -r / "--overlay=bind,/be,$R/f-upper,$R/f-lower" \
    /dev/brd -r /be >"$R/brd-bind.log" 2>&1
status=$?
echo "--- bind brd output ---"
cat "$R/brd-bind.log"
ok "bind brd exits successfully" "$status" "0"
ok "bind brd deduped same" \
    "$(grep -c '^deduped same$' "$R/brd-bind.log")" "1"
ok "bind brd deduped d/same" \
    "$(grep -c '^deduped d/same$' "$R/brd-bind.log")" "1"
ok "bind brd kept d/diff" \
    "$(grep -c '^kept d/diff$' "$R/brd-bind.log")" "1"
ok "identical upper files are gone" \
    "$([ ! -e "$R/f-upper/same" ] && [ ! -e "$R/f-upper/d/same" ] \
        && echo absent || echo present)" "absent"
ok "different upper file remains" \
    "$([ -e "$R/f-upper/d/diff" ] && echo present || echo absent)" "present"

# ------------------------------------------------------------------
# a directory needs -r
# ------------------------------------------------------------------
"$UVROOT" -r / "--overlay=bind,/be,$R/f-upper,$R/f-lower" \
    /dev/brd /be/d >"$R/brd-dir.log" 2>&1
dir_status=$?
ok "plain directory is refused" "$dir_status" "1"
ok "refusal mentions -r" \
    "$(grep -c 'use -r' "$R/brd-dir.log")" "1"

echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
