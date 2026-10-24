#!/bin/sh
# Editors such as Kate save through a temporary file followed by
# rename(2).  In block mode that rename must still produce a sparse delta
# against the existing lower file, not a whole-file copy.
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

mkdir -p "$R/upper" "$R/lower"
dd if=/dev/urandom of="$R/lower/big" bs=1024 count=64 status=none
LOWER_SHA="$(sha256sum "$R/lower/big" | cut -d' ' -f1)"

"$UVROOT" -r / -b "$R/lower/big:/orig" \
    "--overlay=block,/be,$R/upper,$R/lower" \
    /bin/sh -c '
        cp /orig /be/tmp
        dd if=/dev/zero of=/be/tmp bs=1 seek=8192 count=4096 \
            conv=notrunc status=none
        mv /be/tmp /be/big
        /dev/bkc /be/big
        echo bkc:$?
    ' >"$R/run.log" 2>&1
status=$?

echo "--- container output ---"
cat "$R/run.log"
echo "--- database ---"
sed -n '/^B/p' "$R/upper/.overlay"

UPPER_SIZE="$(stat -c %s "$R/upper/big" 2>/dev/null)"
UPPER_BLOCKS="$(stat -c %b "$R/upper/big" 2>/dev/null)"
UPPER_SHA="$(sha256sum "$R/upper/big" 2>/dev/null | cut -d' ' -f1)"
LOWER_AFTER="$(sha256sum "$R/lower/big" | cut -d' ' -f1)"

echo "LOWER_SHA=$LOWER_SHA"
echo "LOWER_AFTER=$LOWER_AFTER"
echo "UPPER_SIZE=$UPPER_SIZE"
echo "UPPER_BLOCKS=$UPPER_BLOCKS"
echo "UPPER_SHA=$UPPER_SHA"

ok "container exits successfully" "$status" "0"
ok "bkc reports unmerged data" \
    "$(grep -c '^bkc:1$' "$R/run.log")" "1"
ok "lower file is unchanged" "$LOWER_AFTER" "$LOWER_SHA"
ok "upper sparse file ends at the modified block" "$UPPER_SIZE" "12288"
ok "upper file only occupies one block" "$UPPER_BLOCKS" "8"
ok "database records only the modified block" \
    "$(grep -c '8192:4096' "$R/upper/.overlay")" "1"
ok "database does not record the whole file" \
    "$(grep -c '0:65536' "$R/upper/.overlay")" "0"
ok "temporary file was removed from type 1" \
    "$([ -e "$R/upper/tmp" ] && echo present || echo absent)" "absent"

echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
