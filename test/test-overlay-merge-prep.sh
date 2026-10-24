#!/bin/sh
# Prepare a block-overlay merge without running bme.
#
#   type 2: random 64 KiB file
#   type 1: only the 4 KiB block modified by the container
#
# The test records the lower hash, the replacement hash, the hash of the
# modified type-1 block and the hash of the expected merged file.  It
# deliberately leaves the block-delta entry in place for a later bme.
set -u

UVROOT="${UVROOT:-$(cd "$(dirname "$0")/.." && pwd)/src/uvroot}"
HELPER="$(dirname "$UVROOT")/overlayctl"
R="${TEST_DIR:-$(mktemp -d)}"
KEEP="${KEEP:-0}"
pass=0
fail=0

cleanup() {
    if [ "$KEEP" = "1" ]; then
        echo "TEST_DIR kept: $R"
    else
        rm -rf "$R"
    fi
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

mkdir -p "$R/upper" "$R/lower"

echo "TEST_DIR=$R"
echo "TYPE1_DIR=$R/upper"
echo "TYPE2_DIR=$R/lower"
echo "DB=$R/upper/.overlay"
echo "EXPECTED=$R/expected"
dd if=/dev/urandom of="$R/lower/data" bs=1024 count=64 status=none
dd if=/dev/urandom of="$R/repl" bs=4096 count=1 status=none

LOWER_SHA="$(sha "$R/lower/data")"
REPL_SHA="$(sha "$R/repl")"

# Expected merged content = lower with the 4 KiB block at offset 16384
# replaced.  This is only recorded here; it is not written to type 1.
cp "$R/lower/data" "$R/expected"
dd if="$R/repl" of="$R/expected" bs=4096 seek=4 count=1 conv=notrunc status=none
EXPECTED_SHA="$(sha "$R/expected")"

"$UVROOT" -r / -b "$R/repl:/repl" \
    "--overlay=block,/mnt/ovl,$R/upper,$R/lower" \
    /bin/sh -c '
        dd if=/repl of=/mnt/ovl/data bs=4096 seek=4 count=1 \
            conv=notrunc status=none
        /dev/bkc /mnt/ovl/data
        echo bkc:$?
    ' >"$R/run.log" 2>&1
run_status=$?

echo "--- container output ---"
cat "$R/run.log"
echo "--- end container output ---"

LOWER_AFTER_SHA="$(sha "$R/lower/data")"
TYPE1_FULL_SHA="$(sha "$R/upper/data")"
TYPE1_BLOCK_SHA="$(dd if="$R/upper/data" bs=4096 skip=4 count=1 \
    status=none | sha256sum | cut -d' ' -f1)"
TYPE1_SIZE="$(stat -c %s "$R/upper/data")"
TYPE1_DU="$(du -k "$R/upper/data" | cut -f1)"
LOWER_BLOCK_SHA="$(dd if="$R/lower/data" bs=4096 skip=4 count=1 \
    status=none | sha256sum | cut -d' ' -f1)"
DB_SHA="$(awk -F'\t' '/^B\tdata\t/ { print $7 }' "$R/upper/.overlay")"

echo "LOWER_SHA=$LOWER_SHA"
echo "REPL_SHA=$REPL_SHA"
echo "TYPE1_FULL_SHA=$TYPE1_FULL_SHA"
echo "TYPE1_MODIFIED_BLOCK_SHA=$TYPE1_BLOCK_SHA"
echo "LOWER_MODIFIED_BLOCK_SHA=$LOWER_BLOCK_SHA"
echo "EXPECTED_MERGED_SHA=$EXPECTED_SHA"
echo "DB_SHA=$DB_SHA"
echo "TYPE1_SIZE=$TYPE1_SIZE"
echo "TYPE1_DU_KB=$TYPE1_DU"
echo "TYPE1_DB:"
sed -n '/^B/p' "$R/upper/.overlay"

ok "container exited successfully" "$run_status" "0"
ok "bkc sees unmerged data" \
    "$(grep -c '^bkc:1$' "$R/run.log")" "1"
ok "bkc hash check passed" \
    "$(grep -c 'unmerged hash-ok' "$R/run.log")" "1"
ok "lower file is unchanged" "$LOWER_AFTER_SHA" "$LOWER_SHA"
ok "type 1 contains the modified block" "$TYPE1_BLOCK_SHA" "$REPL_SHA"
ok "type 1 block differs from the lower block" \
    "$([ "$TYPE1_BLOCK_SHA" != "$LOWER_BLOCK_SHA" ] && echo differs || echo same)" \
    "differs"
ok "database hash equals the expected merged hash" "$DB_SHA" "$EXPECTED_SHA"
ok "type 1 sparse file only reaches the modified block" \
    "$TYPE1_SIZE" "20480"
ok "type 1 is sparse" \
    "$([ "$TYPE1_DU" -lt 16 ] && echo sparse || echo full)" "sparse"
ok "database still has the block entry" \
    "$(grep -c '^B	data	' "$R/upper/.overlay")" "1"
ok "database records the exact modified range" \
    "$(grep -c '16384:4096' "$R/upper/.overlay")" "1"
ok "expected merged hash differs from lower" \
    "$([ "$EXPECTED_SHA" != "$LOWER_SHA" ] && echo differs || echo same)" "differs"
ok "no permanent merge was performed" \
    "$(grep -c '^B	data	' "$R/upper/.overlay")" "1"

echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
