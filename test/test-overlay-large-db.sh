#!/bin/sh
# A file with many non-contiguous modified blocks produces a .overlay
# line larger than the old fixed 8 KiB parser buffer.  The line must
# still load in a second process and bkc must see the delta.
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
dd if=/dev/urandom of="$R/lower/big" bs=1M count=8 status=none

# 1024 separate 4 KiB ranges: contiguous blocks are intentionally left
# untouched, so range coalescing cannot shorten the database line.
"$UVROOT" -r / "--overlay=block,/be,$R/upper,$R/lower" \
    /usr/bin/python3 -c '
p = "/be/big"
f = open(p, "r+b")
for i in range(0, 2048, 2):
    f.seek(i * 4096)
    f.write(b"X" * 4096)
f.close()
' >"$R/write.log" 2>&1
write_status=$?

DB_BYTES="$(wc -c < "$R/upper/.overlay")"
RANGES="$(grep -o ':' "$R/upper/.overlay" | wc -l)"
echo "DB_BYTES=$DB_BYTES"
echo "DB_COLONS=$RANGES"

# The first process only wrote the database.  A second process has to
# parse that long B line from disk.
"$UVROOT" -r / "--overlay=block,/be,$R/upper,$R/lower" \
    /dev/bkc /be/big >"$R/check.log" 2>&1
check_status=$?
echo "--- second-process bkc output ---"
cat "$R/check.log"

ok "writer exits successfully" "$write_status" "0"
ok "database line is larger than 8 KiB" \
    "$([ "$DB_BYTES" -gt 8192 ] && echo large || echo small)" "large"
ok "writer produced many separate ranges" \
    "$([ "$RANGES" -gt 1000 ] && echo many || echo few)" "many"
ok "second process loads the long database" "$check_status" "1"
ok "second process bkc sees unmerged data" \
    "$(grep -c 'unmerged hash-ok' "$R/check.log")" "1"
ok "database still has the block entry" \
    "$(grep -c '^B	big	' "$R/upper/.overlay")" "1"
ok "no merge was attempted" \
    "$(grep -c '^merged big$' "$R/check.log")" "0"

echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
