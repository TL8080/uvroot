#!/bin/sh
# shellcheck disable=SC2086,SC2016
# Regression tests for the two ways a block-backed image can go wrong:
#
#   1. Two containers opening the same writable image at once.  Each one
#      keeps its own copy of the block and inode bitmaps and writes it
#      back, so the filesystem ends up corrupt.  An image is therefore
#      locked exclusively while it is mounted (shared for read-only
#      images) and the second container is refused.
#
#   2. Permissions stored inside the image being ignored.  When the image
#      is the guest root every real process is the same host user, so the
#      host kernel cannot tell the virtual identities apart: vperm has to
#      enforce the image metadata itself.  A file created by the virtual
#      root must not be readable, writable or removable by another id.
#
# A block *data* mount (--img=/mnt/disk:image under a real rootfs) is a
# plain directory tree exposed to a guest running as the real host user:
# the image permissions stay informational there, and writes must keep
# working.
#
# The guest root used below is a fresh ext4 image; the host tool chain is
# bound into it so the tests do not depend on a particular rootfs.
#
# Usage:  UVROOT=/path/to/uvroot sh test/test-image-guard.sh

UVROOT="${UVROOT:-$(cd "$(dirname "$0")/.." && pwd)/src/uvroot}"
WORK="$(mktemp -d)"
BIND="-b /bin -b /sbin -b /lib -b /lib64 -b /usr"

ROOT_IMAGE="$WORK/root.img"
DATA_IMAGE="$WORK/data.img"
LOCK_IMAGE="$WORK/lock.img"

pass=0
fail=0

cleanup() {
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

have() {
    command -v "$1" >/dev/null 2>&1
}

if ! have mke2fs || ! have debugfs || ! have e2fsck; then
    echo "== image guard (skipped, mke2fs/debugfs/e2fsck are needed) =="
    echo
    echo "passed: 0   failed: 0"
    exit 0
fi

make_image() {
    rm -f "$1"
    truncate -s 32M "$1"
    mke2fs -q -t ext4 -F "$1"
}

# As the virtual root, on the image used as the guest root.
as_root() {
    "$UVROOT" -r / "--img=/:$ROOT_IMAGE" --vperm-id=0 $BIND "$@" \
        >/dev/null 2>&1
}

# As another virtual id, on the same image.
as_user() {
    uid="$1"
    shift
    "$UVROOT" -r / "--img=/:$ROOT_IMAGE" "--vperm-id=$uid" $BIND "$@" \
        >/dev/null 2>&1
}

allowed() {
    uid="$1"
    shift
    as_user "$uid" "$@"
}

denied() {
    uid="$1"
    shift
    ! as_user "$uid" "$@"
}

# ------------------------------------------------------------------
# Setup: fixtures owned by the virtual root
# ------------------------------------------------------------------

make_image "$ROOT_IMAGE"

as_root /bin/sh -c '
  mkdir -p /perm/dir777 /perm/rootonly
  chmod 755 /perm; chmod 777 /perm/dir777; chmod 700 /perm/rootonly
  echo ro > /perm/ro; chmod 644 /perm/ro
  echo wr > /perm/wr; chmod 666 /perm/wr
  echo f  > /perm/rootonly/f; chmod 644 /perm/rootonly/f
  printf "#!/bin/sh\necho ran\n" > /perm/rootonly/exec
  chmod 700 /perm/rootonly/exec
' || true

echo "== image as the guest root: permissions =="

check "root can create and chmod" sh -c \
    "'$UVROOT' -r / \"--img=/:$ROOT_IMAGE\" --vperm-id=0 $BIND /bin/ls -l /perm/ro >/dev/null 2>&1"

check "other id reads a 0644 file" allowed 1000 /bin/cat /perm/ro
check "other id cannot write a 0644 file" denied 1000 /bin/sh -c 'echo x > /perm/ro'
check "other id cannot delete a file from a 0755 directory" \
    denied 1000 /bin/rm -f /perm/ro
check "other id cannot read through a 0700 directory" \
    denied 1000 /bin/cat /perm/rootonly/f
check "other id cannot create in a 0755 directory" \
    denied 1000 /bin/sh -c 'echo n > /perm/newfile'
check "other id can create in a 0777 directory" \
    allowed 1000 /bin/sh -c 'echo n > /perm/dir777/new'
check "other id cannot mkdir in a 0755 directory" \
    denied 1000 /bin/mkdir /perm/newdir
check "other id cannot truncate a file it cannot write" \
    denied 1000 /usr/bin/truncate -s 0 /perm/ro
check "other id cannot chmod a file it does not own" \
    denied 1000 /bin/chmod 777 /perm/ro
check "other id cannot rename inside a 0755 directory" \
    denied 1000 /bin/mv /perm/wr /perm/wr2
check "other id cannot delete a 0666 file from a 0755 directory" \
    denied 1000 /bin/rm -f /perm/wr
check "other id cannot access(w) a file it cannot write" \
    denied 1000 /bin/sh -c 'test -w /perm/ro'
check "other id cannot execute a 0700 file" \
    denied 1000 /perm/rootonly/exec

check "root can still write everywhere" sh -c \
    "'$UVROOT' -r / \"--img=/:$ROOT_IMAGE\" --vperm-id=0 $BIND /bin/sh -c '
       echo x > /perm/ro && rm -f /perm/ro && mkdir /perm/d && rmdir /perm/d' \
       >/dev/null 2>&1"

# ------------------------------------------------------------------
# Nested creation through "mkdir -p" must reach the image
# ------------------------------------------------------------------

echo "== image as the guest root: mkdir -p persistence =="

as_root /bin/mkdir -p /deep/a/b || true
check "mkdir -p creates every component in the image" sh -c \
    "debugfs -R 'ls -l /deep/a' '$ROOT_IMAGE' 2>/dev/null | grep -q ' b'"

# ------------------------------------------------------------------
# One writer at a time
# ------------------------------------------------------------------

echo "== one image, several containers =="

# The first container owns the image and serves the others; they never
# open the file, so their block/inode bitmaps cannot race.
make_image "$LOCK_IMAGE"
"$UVROOT" -r / "--img=/:$LOCK_IMAGE" --vperm-id=0 $BIND /bin/sh -c \
    'echo owner > /owner.txt; sleep 20' >/dev/null 2>&1 &
LOCK_PID=$!
sleep 2

check "a second container shares the owner's image" sh -c \
    "'$UVROOT' -r / \"--img=/:$LOCK_IMAGE\" --vperm-id=0 $BIND \
       /bin/cat /owner.txt 2>/dev/null | grep -q owner"

check "a symlink to the image reaches the same owner" sh -c \
    "ln -sf '$LOCK_IMAGE' '$WORK/same-image.img' &&
     '$UVROOT' -r / \"--img=/:$WORK/same-image.img\" --vperm-id=0 $BIND \
       /bin/cat /owner.txt 2>/dev/null | grep -q owner"

check "a second container can write through the owner" sh -c \
    "'$UVROOT' -r / \"--img=/:$LOCK_IMAGE\" --vperm-id=0 $BIND \
       /bin/sh -c 'echo guest > /guest.txt' >/dev/null 2>&1 &&
     '$UVROOT' -r / \"--img=/:$LOCK_IMAGE\" --vperm-id=0 $BIND \
       /bin/cat /guest.txt 2>/dev/null | grep -q guest"

# Two clients writing at the same time must not corrupt anything.
"$UVROOT" -r / "--img=/:$LOCK_IMAGE" --vperm-id=0 $BIND /bin/sh -c \
    'i=0; while [ $i -lt 20 ]; do mkdir -p /w1/d$i; echo a > /w1/d$i/f; i=$((i+1)); done' \
    >/dev/null 2>&1 &
CLIENT_A=$!
"$UVROOT" -r / "--img=/:$LOCK_IMAGE" --vperm-id=0 $BIND /bin/sh -c \
    'i=0; while [ $i -lt 20 ]; do mkdir -p /w2/d$i; echo b > /w2/d$i/f; i=$((i+1)); done' \
    >/dev/null 2>&1 &
CLIENT_B=$!
wait "$CLIENT_A" 2>/dev/null || true
wait "$CLIENT_B" 2>/dev/null || true

check "both concurrent writers landed" sh -c \
    "debugfs -R 'ls -l /w1' '$LOCK_IMAGE' 2>/dev/null | grep -c ' d19' | grep -q 1 &&
     debugfs -R 'ls -l /w2' '$LOCK_IMAGE' 2>/dev/null | grep -c ' d19' | grep -q 1"

wait "$LOCK_PID" 2>/dev/null || true

check "the shared image is not corrupted" sh -c \
    "e2fsck -fn '$LOCK_IMAGE' >/dev/null 2>&1"

check "the image can be owned again afterwards" sh -c \
    "'$UVROOT' -r / \"--img=/:$LOCK_IMAGE\" --vperm-id=0 $BIND \
       /bin/cat /owner.txt 2>/dev/null | grep -q owner"

echo "== the service survives its owner =="

# The owner exits while a client is still using the image: the client
# must take over instead of getting I/O errors.
"$UVROOT" -r / "--img=/:$LOCK_IMAGE" --vperm-id=0 $BIND \
    /bin/sh -c 'echo alive > /survive.txt; sleep 4' >/dev/null 2>&1 &
SURVIVOR=$!
sleep 2

check "a client keeps working after the owner exited" sh -c \
    "'$UVROOT' -r / \"--img=/:$LOCK_IMAGE\" --vperm-id=0 $BIND /bin/sh -c \
       'cat /survive.txt; sleep 5; cat /survive.txt && echo POST-EXIT-OK' \
       2>/dev/null | grep -q POST-EXIT-OK"

wait "$SURVIVOR" 2>/dev/null || true

# ------------------------------------------------------------------
# Two image files sharing disk blocks (reflink, snapshot)
# ------------------------------------------------------------------

echo "== images sharing disk blocks =="

truncate -s 16M "$WORK/blocks-a.img"
mke2fs -q -t ext4 -F "$WORK/blocks-a.img" 2>/dev/null

if cp --reflink=always "$WORK/blocks-a.img" "$WORK/blocks-b.img" 2>/dev/null; then
    "$UVROOT" -r / "--img=$WORK/blocks-a:$WORK/blocks-a.img" --vperm-id=0 \
	$BIND /bin/sleep 12 >/dev/null 2>&1 &
    SHARED_PID=$!
    sleep 2

    check "an image sharing blocks with a mounted one is refused" sh -c \
	"'$UVROOT' -r / \"--img=$WORK/blocks-b:$WORK/blocks-b.img\" \
	   --vperm-id=0 $BIND /bin/true 2>&1 | grep -q 'share disk blocks'"

    wait "$SHARED_PID" 2>/dev/null || true

    cp --reflink=never "$WORK/blocks-a.img" "$WORK/blocks-c.img"
    check "an independent copy of the image is accepted" sh -c \
	"! '$UVROOT' -r / \"--img=$WORK/blocks-c:$WORK/blocks-c.img\" \
	   --vperm-id=0 $BIND /bin/true 2>&1 | grep -q 'share disk blocks'"
else
    printf '  skip %s\n' "images sharing disk blocks (no reflink here)"
fi

# ------------------------------------------------------------------
# Read-only images
# ------------------------------------------------------------------

echo "== read-only images =="

truncate -s 32M "$WORK/ro.img"
mke2fs -q -t ext4 -F "$WORK/ro.img" 2>/dev/null

check "a write is refused as id0 with --read-only" sh -c \
    "! '$UVROOT' -r / --read-only \"--img=$WORK/romnt:$WORK/ro.img\" \
       --vperm-id=0 $BIND /bin/sh -c 'echo x > $WORK/romnt/f' 2>/dev/null"

check "a mkdir is refused with --read-only" sh -c \
    "! '$UVROOT' -r / \"--img=$WORK/romnt:$WORK/ro.img\" --read-only \
       --vperm-id=0 $BIND /bin/mkdir $WORK/romnt/d 2>/dev/null"

check "nothing reached the read-only image" sh -c \
    "! debugfs -R 'ls -l /' '$WORK/ro.img' 2>/dev/null | grep -qE ' (f|d)$'"

# The same operations do work without the flag: that file is then used to
# show that --read-only also refuses to remove something.
check "the same write succeeds without --read-only" sh -c \
    "'$UVROOT' -r / \"--img=$WORK/romnt:$WORK/ro.img\" --vperm-id=0 $BIND \
       /bin/sh -c 'echo x > $WORK/romnt/keep' 2>/dev/null"

check "an unlink is refused with --read-only" sh -c \
    "! '$UVROOT' -r / --read-only \"--img=$WORK/romnt:$WORK/ro.img\" \
       --vperm-id=0 $BIND /bin/rm -f $WORK/romnt/keep 2>/dev/null"

check "the file is still there" sh -c \
    "debugfs -R 'ls -l /' '$WORK/ro.img' 2>/dev/null | grep -qE ' keep$'"

# ------------------------------------------------------------------
# Per-mapping read-only
# ------------------------------------------------------------------

echo "== per-mapping read-only =="

mkdir -p "$WORK/rwdir" "$WORK/rodir"

check "--ro refuses writes on one -b binding only" sh -c \
    "'$UVROOT' -r / -b $WORK/rwdir:/rw -b $WORK/rodir:/ro --ro=/ro $BIND \
       /bin/sh -c 'echo x > /rw/f' 2>/dev/null &&
     ! '$UVROOT' -r / -b $WORK/rwdir:/rw -b $WORK/rodir:/ro --ro=/ro $BIND \
       /bin/sh -c 'echo x > /ro/f' 2>/dev/null"

check "the writable binding received the file" sh -c \
    "test -f '$WORK/rwdir/f' && ! test -e '$WORK/rodir/f'"

check "--ro=/ refuses writes on the -r rootfs but not reads" sh -c \
    "! '$UVROOT' -r / --ro=/ $BIND /bin/sh -c 'echo x > $WORK/nope' 2>/dev/null &&
     '$UVROOT' -r / --ro=/ $BIND /bin/cat /etc/hostname >/dev/null 2>&1"

# The rule is about paths, not mounts: it covers everything below the
# path, and every process of the container.
mkdir -p "$WORK/tree/sub/deeper"
echo v > "$WORK/tree/sub/deeper/keep"

check "--ro covers sub-directories and child processes" sh -c \
    "! '$UVROOT' -r / -b $WORK/tree:/tree --ro=/tree $BIND \
       /bin/sh -c 'rm -f /tree/sub/deeper/keep; /bin/mkdir /tree/sub/deeper/d' \
       2>/dev/null &&
     test -f '$WORK/tree/sub/deeper/keep'"

mkdir -p "$WORK/locked"
check "--ro works on a plain directory, without its own mount" sh -c \
    "! '$UVROOT' -r / --ro=$WORK/locked $BIND \
       /bin/sh -c 'echo x > $WORK/locked/f' 2>/dev/null &&
     ! test -e '$WORK/locked/f' &&
      '$UVROOT' -r / --ro=$WORK/locked $BIND /bin/ls $WORK/locked >/dev/null 2>&1"

# A rule names a path to protect, not a path to reserve: if it does not
# exist when the container starts, it is ignored (with a warning).
check "--ro on a missing path is ignored, with a warning" sh -c \
    "'$UVROOT' -r / -b $WORK/tree:/tree --ro=/tree/nope $BIND \
       /bin/mkdir /tree/nope 2>&1 | grep -q 'is ignored' &&
     test -d '$WORK/tree/nope'"

# Once installed, the rule survives an outside removal: the path may not
# be recreated from inside the container.
mkdir -p "$WORK/tree/recreate"
"$UVROOT" -r / -b "$WORK/tree:/tree" --ro=/tree/recreate $BIND \
    /bin/sh -c 'sleep 2; /bin/mkdir /tree/recreate 2>&1; echo "rc=$?"' \
    > "$WORK/recreate.out" 2>&1 &
RECREATE_PID=$!
sleep 1
rmdir "$WORK/tree/recreate"
wait "$RECREATE_PID" 2>/dev/null || true

check "a read-only path removed meanwhile cannot be recreated" sh -c \
    "grep -q 'rc=1' '$WORK/recreate.out' &&
     ! test -e '$WORK/tree/recreate'"

truncate -s 32M "$WORK/pm-a.img" "$WORK/pm-b.img"
mke2fs -q -t ext4 -F "$WORK/pm-a.img" 2>/dev/null
mke2fs -q -t ext4 -F "$WORK/pm-b.img" 2>/dev/null

check "--ro picks one image among several" sh -c \
    "'$UVROOT' -r / \"--img=$WORK/pma:$WORK/pm-a.img\" \
       \"--img=$WORK/pmb:$WORK/pm-b.img\" --ro=$WORK/pmb $BIND \
       /bin/sh -c 'echo x > $WORK/pma/f' 2>/dev/null &&
     ! '$UVROOT' -r / \"--img=$WORK/pma:$WORK/pm-a.img\" \
       \"--img=$WORK/pmb:$WORK/pm-b.img\" --ro=$WORK/pmb $BIND \
       /bin/sh -c 'echo x > $WORK/pmb/f' 2>/dev/null"

# ------------------------------------------------------------------
# Writing past the end of the image
# ------------------------------------------------------------------

echo "== writing past the end of the image =="

truncate -s 64M "$WORK/tiny.img"
mke2fs -q -t ext4 -F "$WORK/tiny.img" 2>/dev/null
guard_before=$(df -k --output=avail "$WORK" | tail -1)

if "$UVROOT" -r / "--img=$WORK/tiny:$WORK/tiny.img" /bin/dd \
	if=/dev/zero of="$WORK/tiny/fill" bs=1M count=200 >/dev/null 2>&1; then
    printf '  FAIL %s\n' "writing past the image fails instead of succeeding"
    fail=$((fail + 1))
else
    printf '  ok   %s\n' "writing past the image fails instead of succeeding"
    pass=$((pass + 1))
fi

check "the half-written file was rolled back" sh -c \
    "! debugfs -R 'ls -l /' '$WORK/tiny.img' 2>/dev/null | grep -q fill"

check "the image is still consistent after ENOSPC" sh -c \
    "e2fsck -fn '$WORK/tiny.img' >/dev/null 2>&1"

guard_after=$(df -k --output=avail "$WORK" | tail -1)

# The image itself legitimately fills up (64 MiB); what must not happen
# is the host keeping the 200 MiB that did not fit in it.
if [ $((guard_before - guard_after)) -lt 81920 ]; then
    printf '  ok   %s\n' "the host keeps no copy of the data that did not fit"
    pass=$((pass + 1))
else
    printf '  FAIL %s (%s KiB)\n' \
	"the host keeps no copy of the data that did not fit" \
	"$((guard_before - guard_after))"
    fail=$((fail + 1))
fi

# ------------------------------------------------------------------
# An image nested inside a mounted one
# ------------------------------------------------------------------

echo "== nested images =="

mkdir -p "$WORK/outer-seed"
truncate -s 8M "$WORK/inner.img"
mke2fs -q -t ext4 -F -d "$WORK/outer-seed" "$WORK/inner.img" 2>/dev/null
cp "$WORK/inner.img" "$WORK/outer-seed/inner.img"
truncate -s 32M "$WORK/outer.img"
mke2fs -q -t ext4 -F -d "$WORK/outer-seed" "$WORK/outer.img" 2>/dev/null

"$UVROOT" -r / "--img=$WORK/outer:$WORK/outer.img" --vperm-id=0 $BIND \
    /bin/sh -c "ls -l $WORK/outer/ >/dev/null; \
		cat $WORK/outer/inner.img >/dev/null; sleep 15" \
    >/dev/null 2>&1 &
NESTED_PID=$!
sleep 2

USER_DIR=${XDG_RUNTIME_DIR:-/tmp/.uvroot-share-$(id -u)}
# Each marker's first line is the mirror path (the following lines
# hold the image path, its device and its inode).
CACHE=$(for f in "$USER_DIR"/caches/*; do head -1 "$f"; done 2>/dev/null | tail -1)

check "an image inside a mounted image is refused" sh -c \
    "'$UVROOT' -r / \"--img=$WORK/inner:$CACHE/inner.img\" --vperm-id=0 $BIND \
       /bin/true 2>&1 | grep -q 'lives inside'"

wait "$NESTED_PID" 2>/dev/null || true
check "the outer image survived" sh -c "e2fsck -fn '$WORK/outer.img' >/dev/null 2>&1"

# ------------------------------------------------------------------
# A block data mount stays writable
# ------------------------------------------------------------------

echo "== block data mount =="

make_image "$DATA_IMAGE"
check "write a new file" sh -c \
    "'$UVROOT' -r / \"--img=$WORK/mnt:$DATA_IMAGE\" \
       /bin/sh -c 'echo data > $WORK/mnt/x.txt' >/dev/null 2>&1"

check "the data mount host directory sees it" sh -c \
    "debugfs -R 'cat /x.txt' '$DATA_IMAGE' 2>/dev/null | grep -q data"

check "mkdir -p on a data mount persists" sh -c \
    "'$UVROOT' -r / \"--img=$WORK/mnt:$DATA_IMAGE\" \
       /bin/mkdir -p $WORK/mnt/a/b >/dev/null 2>&1 &&
     debugfs -R 'ls -l /a' '$DATA_IMAGE' 2>/dev/null | grep -q ' b'"

echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
