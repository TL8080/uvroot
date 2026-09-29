#!/bin/sh
# Several containers hosted as threads of one process (--multi).
# shellcheck disable=SC2086,SC2016

UVROOT=${UVROOT:-uvroot}

passed=0
failed=0

ok() {
    name=$1
    shift
    if "$@" >/dev/null 2>&1; then
	printf '  ok   %s\n' "$name"
	passed=$((passed + 1))
    else
	printf '  FAIL %s\n' "$name"
	failed=$((failed + 1))
    fi
}

echo "== one process, several containers =="

check() {
    name=$1
    shift
    out=$("$@" 2>/dev/null) || true
    case "$out" in
	*"$EXPECT"*) printf '  ok   %s\n' "$name"; passed=$((passed + 1)) ;;
	*) printf '  FAIL %s (got: %s)\n' "$name" "$out"; failed=$((failed + 1)) ;;
    esac
}

EXPECT=one
check "the first container runs" sh -c \
    "'$UVROOT' --multi -- -r / /bin/sh -c 'echo one' -- -r / /bin/sh -c 'echo two'"

EXPECT=two
check "the second container runs" sh -c \
    "'$UVROOT' --multi -- -r / /bin/sh -c 'echo one' -- -r / /bin/sh -c 'echo two'"

# Every container must keep its own tracee tree: the children of one
# container must not be mistaken for the other's.
EXPECT=A2
check "a container keeps its own fork tree" sh -c \
    "'$UVROOT' --multi -- -r / /bin/sh -c '/bin/echo A1; /bin/echo A2' \
       -- -r / /bin/sh -c '/bin/echo B1; /bin/echo B2'"

EXPECT=B2
check "the other container keeps its own fork tree" sh -c \
    "'$UVROOT' --multi -- -r / /bin/sh -c '/bin/echo A1; /bin/echo A2' \
       -- -r / /bin/sh -c '/bin/echo B1; /bin/echo B2'"

EXPECT=5
check "the exit status of the last failing container is returned" sh -c \
    "'$UVROOT' --multi -- -r / /bin/sh -c 'exit 3' -- -r / /bin/sh -c 'exit 5' \
       2>/dev/null; echo \$?"

ok "a single container is unaffected" sh -c \
    "'$UVROOT' -r / /bin/echo single >/dev/null 2>&1"

# They must share the process, not run one after the other.
start=$(date +%s)
"$UVROOT" --multi -- -r / /bin/sleep 2 -- -r / /bin/sleep 2 >/dev/null 2>&1
end=$(date +%s)

if [ $((end - start)) -le 3 ]; then
    printf '  ok   %s\n' "the containers run in parallel"
    passed=$((passed + 1))
else
    printf '  FAIL %s (%s s)\n' "the containers run in parallel" "$((end - start))"
    failed=$((failed + 1))
fi

echo "== repeated starts =="

# Hosting several containers relies on thread-safe allocation: a global
# leak-tracking list used to corrupt the heap intermittently, so make
# sure a batch of starts all succeed.
good_runs=0
run=0
while [ "$run" -lt 6 ]; do
    if "$UVROOT" --multi -- -r / /bin/sleep 1 -- -r / /bin/sleep 1 \
	>/dev/null 2>&1; then
	good_runs=$((good_runs + 1))
    fi
    run=$((run + 1))
done

if [ "$good_runs" -eq 6 ]; then
    printf '  ok   %s\n' "six consecutive multi-container starts succeed"
    passed=$((passed + 1))
else
    printf '  FAIL %s (%s/6)\n' \
	"six consecutive multi-container starts succeed" "$good_runs"
    failed=$((failed + 1))
fi

echo "== a fatal signal stops every container =="

"$UVROOT" --multi -- -r / /bin/sleep 30 -- -r / /bin/sleep 30 >/dev/null 2>&1 &
SUPERVISOR=$!
sleep 3

# Only the tracees of this supervisor, so that unrelated processes on
# the host cannot skew the count.
before=$(pgrep -P "$SUPERVISOR" -x sleep | wc -l)
kill -QUIT "$SUPERVISOR" 2>/dev/null
wait "$SUPERVISOR" 2>/dev/null || true
sleep 1
after=$(pgrep -P "$SUPERVISOR" -x sleep | wc -l)

if [ "$before" -ge 2 ] && [ "$after" -eq 0 ]; then
    printf '  ok   %s\n' "SIGQUIT stopped the tracees of both containers"
    passed=$((passed + 1))
else
    printf '  FAIL %s (before=%s after=%s)\n' \
	"SIGQUIT stopped the tracees of both containers" "$before" "$after"
    failed=$((failed + 1))
fi

echo "== one image shared by two containers of one process =="

WORK=$(mktemp -d)
truncate -s 32M "$WORK/disk.img"
mke2fs -q -t ext4 -F "$WORK/disk.img"

EXPECT=shared
check "a container reads what the other wrote, same image" sh -c \
    "'$UVROOT' --multi \
       -- -r / \"--img=$WORK/m1:$WORK/disk.img\" --vperm-id=0 \
            /bin/sh -c 'echo shared > $WORK/m1/from1.txt' \
       -- -r / \"--img=$WORK/m2:$WORK/disk.img\" --vperm-id=0 \
            /bin/sh -c 'sleep 1; cat $WORK/m2/from1.txt' 2>/dev/null"

# Both containers writing at once must land, and must not corrupt the
# filesystem: they share one driver inside one process.
"$UVROOT" --multi \
    -- -r / "--img=$WORK/m1:$WORK/disk.img" --vperm-id=0 /bin/sh -c \
	'i=0; while [ $i -lt 25 ]; do mkdir -p '"$WORK"'/m1/a/d$i; echo a > '"$WORK"'/m1/a/d$i/f; i=$((i+1)); done' \
    -- -r / "--img=$WORK/m2:$WORK/disk.img" --vperm-id=0 /bin/sh -c \
	'i=0; while [ $i -lt 25 ]; do mkdir -p '"$WORK"'/m2/b/d$i; echo b > '"$WORK"'/m2/b/d$i/f; i=$((i+1)); done' \
    >/dev/null 2>&1 || true

count_a=$(debugfs -R 'ls -l /a' "$WORK/disk.img" 2>/dev/null | grep -c 'd[0-9]')
count_b=$(debugfs -R 'ls -l /b' "$WORK/disk.img" 2>/dev/null | grep -c 'd[0-9]')

if [ "$count_a" = 25 ] && [ "$count_b" = 25 ]; then
    printf '  ok   %s\n' "both containers wrote through the shared driver"
    passed=$((passed + 1))
else
    printf '  FAIL %s (a=%s b=%s)\n' \
	"both containers wrote through the shared driver" "$count_a" "$count_b"
    failed=$((failed + 1))
fi

if e2fsck -fn "$WORK/disk.img" >/dev/null 2>&1; then
    printf '  ok   %s\n' "the shared image is still consistent"
    passed=$((passed + 1))
else
    printf '  FAIL %s\n' "the shared image is still consistent"
    failed=$((failed + 1))
fi

# The virtual identity of the creating container must be written into
# the image, even when it borrows the mount of another container.
"$UVROOT" --multi \
    -- -r / "--img=$WORK/m1:$WORK/disk.img" --vperm-id=0 /bin/sh -c \
	'mkdir -p '"$WORK"'/m1/pub; chmod 777 '"$WORK"'/m1/pub; \
	 echo a > '"$WORK"'/m1/pub/root.txt; sleep 3' \
    -- -r / "--img=$WORK/m2:$WORK/disk.img" --vperm-id=1000 /bin/sh -c \
	'sleep 1; echo b > '"$WORK"'/m2/pub/user.txt' >/dev/null 2>&1 || true

owner_root=$(debugfs -R 'stat /pub/root.txt' "$WORK/disk.img" 2>/dev/null |
	     awk '/^User:/{print $2}')
owner_user=$(debugfs -R 'stat /pub/user.txt' "$WORK/disk.img" 2>/dev/null |
	     awk '/^User:/{print $2}')

if [ "$owner_root" = 0 ] && [ "$owner_user" = 1000 ]; then
    printf '  ok   %s\n' "each container's identity is written into the image"
    passed=$((passed + 1))
else
    printf '  FAIL %s (root=%s user=%s)\n' \
	"each container's identity is written into the image" \
	"$owner_root" "$owner_user"
    failed=$((failed + 1))
fi

# One container must not keep objects another one removed: the shared
# mount has to re-list (and drop vanished entries) instead of trusting
# its own cached tree.
"$UVROOT" --multi \
    -- -r / "--img=$WORK/m1:$WORK/disk.img" --vperm-id=0 /bin/sh -c \
	'mkdir '"$WORK"'/m1/bc '"$WORK"'/m1/bb; sleep 3; \
	 rmdir '"$WORK"'/m1/bb; sleep 6' \
    -- -r / "--img=$WORK/m2:$WORK/disk.img" --vperm-id=1001 /bin/sh -c \
	'sleep 1; ls '"$WORK"'/m2 > '"$WORK"'/first; \
	 sleep 4; ls '"$WORK"'/m2 > '"$WORK"'/second' >/dev/null 2>&1 || true

if grep -q '^bb$' "$WORK/first" && grep -q '^bc$' "$WORK/first" \
    && ! grep -q '^bb$' "$WORK/second" && grep -q '^bc$' "$WORK/second"; then
    printf '  ok   %s\n' "a removal in one container is seen by the other"
    passed=$((passed + 1))
else
    printf '  FAIL %s (first=[%s] second=[%s])\n' \
	"a removal in one container is seen by the other" \
	"$(tr '\n' ' ' < "$WORK/first")" "$(tr '\n' ' ' < "$WORK/second")"
    failed=$((failed + 1))
fi

# A large file written and read back through the shared driver must be
# bit-for-bit identical.
head -c 8388608 /dev/urandom > "$WORK/rand.bin"
source_sum=$(md5sum "$WORK/rand.bin" | cut -d' ' -f1)

"$UVROOT" --multi \
    -- -r / "--img=$WORK/m1:$WORK/disk.img" /bin/sh -c \
	"cp $WORK/rand.bin $WORK/m1/r1.bin; md5sum $WORK/m1/r1.bin" \
    -- -r / "--img=$WORK/m2:$WORK/disk.img" /bin/sh -c \
	"cp $WORK/rand.bin $WORK/m2/r2.bin; md5sum $WORK/m2/r2.bin" \
    > "$WORK/sums" 2>/dev/null || true

if [ "$(grep -c "^$source_sum" "$WORK/sums")" = 2 ]; then
    printf '  ok   %s\n' "8 MiB copies stay identical through the shared driver"
    passed=$((passed + 1))
else
    printf '  FAIL %s\n' "8 MiB copies stay identical through the shared driver"
    failed=$((failed + 1))
fi

if e2fsck -fn "$WORK/disk.img" >/dev/null 2>&1; then
    printf '  ok   %s\n' "the image is still clean after the large I/O"
    passed=$((passed + 1))
else
    printf '  FAIL %s\n' "the image is still clean after the large I/O"
    failed=$((failed + 1))
fi

rm -rf "$WORK"

echo
echo "passed: $passed   failed: $failed"
[ "$failed" -eq 0 ]
