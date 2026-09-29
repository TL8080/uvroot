#!/bin/sh
# End-to-end verification of the netfs block backends: raw images, QCOW2
# images, NBD exports and iSCSI LUNs.  A block backend must expose the
# filesystem stored inside the disk, keep ownership and permissions
# there (never in a .uvroot-vperm database), and support symlinks, hard
# links and namespace operations.
#
# Usage:  UVROOT=/path/to/uvroot sh test/test-block-backends.sh
#
# NBD is tested against a local qemu-nbd when available.  iSCSI needs a
# reachable target; set ISCSI_URI=iscsi://host[:port]/iqn/lun and
# ISCSI_IMAGE=<file backing the LUN> to have it exercised, plus
# ISCSI_ATTACH_CMD=<command> to (re)attach the LUN to the freshly
# created image.  Otherwise that section is skipped.  Every section also
# needs mke2fs and debugfs to build and inspect the test image.

UVROOT="${UVROOT:-$(cd "$(dirname "$0")/.." && pwd)/src/uvroot}"
WORK="$(mktemp -d)"
NBD_PID=""

pass=0
fail=0

cleanup() {
    if [ -n "$NBD_PID" ]; then
        kill "$NBD_PID" 2>/dev/null || true
        wait "$NBD_PID" 2>/dev/null || true
    fi
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

section() {
    echo "== $1 =="
}

# ------------------------------------------------------------------
# Test image
# ------------------------------------------------------------------

make_raw_image() {
    path="$1"
    seed="$WORK/seed"

    rm -rf "$seed"
    mkdir -p "$seed/sub" "$seed/emptydir"
    printf 'hello from block\n' >"$seed/hello.txt"
    printf 'nested file\n' >"$seed/sub/nested.txt"
    printf '0123456789' >"$seed/ten.bin"

    rm -f "$path"
    truncate -s 16M "$path"
    mke2fs -q -t ext4 -F -d "$seed" "$path"
}

# A symlink whose target contains ".." is stored by GNU tar as a mode-000
# placeholder file that is later replaced by the real symlink.
unsafe_seed="$WORK/unsafe-seed"
mkdir -p "$unsafe_seed/etc"
ln -s ../proc/mounts "$unsafe_seed/etc/mtab"
tar -cpf "$WORK/unsafe.tar" -C "$unsafe_seed" .

# In-place copy of a QCOW2 image turned into something debugfs can read.
raw_view() {
    case "$1" in
    *.qcow2)
        qemu-img convert -f qcow2 -O raw "$1" "$WORK/view.raw" || return 1
        printf '%s\n' "$WORK/view.raw"
        ;;
    *)
        printf '%s\n' "$1"
        ;;
    esac
}

image_cat() {
    raw="$(raw_view "$1")" || return 1
    debugfs -R "cat $2" "$raw" 2>/dev/null
}

image_stat_mode() {
    raw="$(raw_view "$1")" || return 1
    debugfs -R "stat $2" "$raw" 2>/dev/null \
        | sed -n 's/.*Mode: *\([0-7]*\).*/\1/p'
}

image_stat_inode() {
    raw="$(raw_view "$1")" || return 1
    debugfs -R "stat $2" "$raw" 2>/dev/null \
        | sed -n 's/^Inode: *\([0-9]*\).*/\1/p'
}

image_link_target() {
    raw="$(raw_view "$1")" || return 1
    debugfs -R "stat $2" "$raw" 2>/dev/null \
        | sed -n 's/.*Fast link dest: "\(.*\)"/\1/p'
}

image_has_entry() {
    raw="$(raw_view "$1")" || return 1
    debugfs -R 'ls -l /' "$raw" 2>/dev/null | awk '{print $NF}' | grep -qx "$2"
}

guest() {
    "$UVROOT" -r / "$OPTION" "$@" 2>/dev/null
}

# ------------------------------------------------------------------
# The individual checks.  They use $OPTION, $IMAGE and $MNT, which are
# set by verify_rw() before each group.
# ------------------------------------------------------------------

check_read_file() {
    [ "$(guest /bin/cat "$MNT/hello.txt")" = "hello from block" ]
}

check_read_nested() {
    [ "$(guest /bin/cat "$MNT/sub/nested.txt")" = "nested file" ]
}

check_stat_size() {
    [ "$(guest /bin/stat -c %s "$MNT/ten.bin")" = "10" ]
}

check_binary_roundtrip() {
    guest /bin/sh -c "cat $MNT/ten.bin > $MNT/copy.bin" || return 1
    [ "$(guest /bin/cat "$MNT/copy.bin")" = "0123456789" ]
}

check_create_file() {
    guest /bin/sh -c "echo written > $MNT/new.txt" || return 1
    [ "$(image_cat "$IMAGE" /new.txt)" = "written" ]
}

check_create_dir() {
    guest /bin/mkdir "$MNT/dir" || return 1
    image_has_entry "$IMAGE" dir
}

check_rename() {
    guest /bin/mv "$MNT/new.txt" "$MNT/renamed.txt" || return 1
    [ "$(image_cat "$IMAGE" /renamed.txt)" = "written" ]
}

check_unlink() {
    guest /bin/rm "$MNT/renamed.txt" || return 1
    ! image_has_entry "$IMAGE" renamed.txt
}

check_rmdir() {
    guest /bin/rmdir "$MNT/dir" || return 1
    ! image_has_entry "$IMAGE" dir
}

check_relative_symlink() {
    guest /bin/ln -s hello.txt "$MNT/rel_link" || return 1
    [ "$(image_link_target "$IMAGE" /rel_link)" = "hello.txt" ] || return 1
    [ "$(guest /bin/cat "$MNT/rel_link")" = "hello from block" ]
}

check_absolute_symlink() {
    guest /bin/ln -s /etc/hostname "$MNT/abs_link" || return 1
    [ "$(image_link_target "$IMAGE" /abs_link)" = "/etc/hostname" ]
}

check_hard_link() {
    guest /bin/ln "$MNT/hello.txt" "$MNT/hard_link" || return 1
    [ "$(image_stat_inode "$IMAGE" /hello.txt)" \
        = "$(image_stat_inode "$IMAGE" /hard_link)" ]
}

# The guest must see a hard link as one file too.  A fresh process
# re-materializes the directory from the image, and two independent
# cache copies used to show nlink 1 and let a write through one name
# leave the other stale.
check_hard_link_visible() {
    guest /bin/sh -c "echo linked > $MNT/hl_src" || return 1
    guest /bin/ln "$MNT/hl_src" "$MNT/hl_dst" || return 1
    guest /bin/sh -c "echo more >> $MNT/hl_src" || return 1
    [ "$(guest /bin/stat -c %h "$MNT/hl_dst")" = "2" ] || return 1
    [ "$(guest /bin/cat "$MNT/hl_dst")" = "$(guest /bin/cat "$MNT/hl_src")" ]
}

check_chmod() {
    guest /bin/chmod 700 "$MNT/hello.txt" || return 1
    [ "$(image_stat_mode "$IMAGE" /hello.txt)" = "0700" ]
}

# The mode of a file the guest creates through open(2)'s mode argument
# must reach the image: the image used to hard-code 0644, which made
# every extracted program non-executable.
check_open_mode() {
    guest /bin/cp /bin/true "$MNT/exec_open" || return 1
    [ "$(image_stat_mode "$IMAGE" /exec_open)" = "0755" ]
}

# A file the guest makes unreadable (mode 0000) still has to be stored:
# GNU tar writes exactly that kind of placeholder for the symlinks it
# considers unsafe, and reading the mirror back used to fail with EACCES
# and abort the whole extraction.
check_unreadable_file_is_stored() {
    guest /bin/sh -c "umask 777; : > $MNT/private" || return 1
    [ "$(image_stat_mode "$IMAGE" /private)" = "0000" ]
}

# End to end: extracting a symlink whose target contains ".." makes tar
# go through a mode-000 placeholder file, then replace it with the real
# symlink.  Both steps have to reach the image.
check_tar_unsafe_symlink() {
    guest /bin/tar -xpf "$WORK/unsafe.tar" -C "$MNT" || return 1
    [ "$(image_link_target "$IMAGE" /etc/mtab)" = "../proc/mounts" ]
}

check_no_vperm_db() {
    ! image_has_entry "$IMAGE" .uvroot-vperm
}

# df(1) must see the image, not the host filesystem that happens to hold
# it: /proc/mounts is synthesized and statfs(2) is answered from the
# filesystem inside the disk.
check_df_reports_image() {
    raw="$(raw_view "$IMAGE")" || return 1
    blocks=$(dumpe2fs -h "$raw" 2>/dev/null | sed -n 's/^Block count: *//p')
    bsize=$(dumpe2fs -h "$raw" 2>/dev/null | sed -n 's/^Block size: *//p')
    [ -n "$blocks" ] && [ -n "$bsize" ] || return 1

    reported=$(guest /bin/df -P -k "$MNT" | awk 'NR == 2 { print $2 }')
    [ "$reported" = "$((blocks * bsize / 1024))" ]
}

# The synthesized mount table names the backend by kind and points at the
# guest mount point.
check_proc_mounts_entry() {
    guest /bin/sh -c "grep -F ' $MNT ' /proc/mounts | grep -q -F '$FSTYPE'"
}

# A -b binding is a host path mapping, so it stays hostfs: the capacity
# is the host filesystem's, and the "used" column is the size of the
# bound file itself.
check_bound_object_size() {
    # Only a raw ext2/3/4 image has a size that maps cleanly onto the
    # host block size; skip the QCOW2 backing file.
    dumpe2fs -h "$IMAGE" >/dev/null 2>&1 || return 0
    bytes=$(stat -c %s "$IMAGE")
    [ -n "$bytes" ] || return 1

    used=$("$UVROOT" -r / "$OPTION" -b "$IMAGE:/bound.img" \
        /bin/df -P -k /bound.img 2>/dev/null | awk 'NR == 2 { print $3 }')
    [ "$used" = "$((bytes / 1024))" ] || return 1

    "$UVROOT" -r / "$OPTION" -b "$IMAGE:/bound.img" \
        /bin/sh -c "grep -F ' /bound.img ' /proc/mounts" 2>/dev/null \
        | grep -q -F hostfs
}

check_compressed_write() {
    guest /bin/sh -c "echo rewritten > $MNT/hello.txt" || return 1
    qemu-img check "$IMAGE" >/dev/null 2>&1 || return 1
    [ "$(image_cat "$IMAGE" /hello.txt)" = "rewritten" ]
}

# verify_rw OPTION IMAGE
verify_rw() {
    OPTION="$1"
    IMAGE="$2"
    MNT=/mnt/disk

    case "$OPTION" in
    *iscsi* | *nbd*) FSTYPE=nblockfs ;;
    *qcow2* | *img* | *raw* | *file*) FSTYPE=imgfs ;;
    *) FSTYPE=netfs ;;
    esac

    check "read file" check_read_file
    check "read nested file" check_read_nested
    check "stat size" check_stat_size
    check "binary round trip" check_binary_roundtrip
    check "create file" check_create_file
    check "create directory" check_create_dir
    check "rename" check_rename
    check "unlink" check_unlink
    check "rmdir" check_rmdir
    check "relative symlink" check_relative_symlink
    check "absolute symlink target" check_absolute_symlink
    check "hard link shares the inode" check_hard_link
    check "hard link is visible to the guest" check_hard_link_visible
    check "chmod is written to the image" check_chmod
    check "open(2) mode reaches the image" check_open_mode
    check "a mode-000 file is still stored" check_unreadable_file_is_stored
    check "tar unsafe-symlink placeholder round trip" check_tar_unsafe_symlink
    check "no .uvroot-vperm database in the image" check_no_vperm_db
    check "df reports the image size" check_df_reports_image
    check "/proc/mounts names the mount" check_proc_mounts_entry
    check "a bound file stays hostfs" check_bound_object_size
}

# ------------------------------------------------------------------
# Backends
# ------------------------------------------------------------------

section "tooling"
if ! have mke2fs || ! have debugfs; then
    echo "mke2fs and debugfs are required; skipping the block backend suite"
    exit 0
fi

RAW="$WORK/raw.img"
section "img (raw)"
make_raw_image "$RAW"
verify_rw "--img=/mnt/disk:$RAW" "$RAW"

FORCED="$WORK/forced.img"
section "img with the generic io_manager"
make_raw_image "$FORCED"
UVROOT_NETFS_FORCE_CUSTOM_IO=1 verify_rw "--img=/mnt/disk:$FORCED" "$FORCED"

if have qemu-img; then
    QCOW="$WORK/disk.qcow2"
    section "qcow2"
    make_raw_image "$WORK/qcow-src.img"
    qemu-img convert -f raw -O qcow2 "$WORK/qcow-src.img" "$QCOW"
    verify_rw "--qcow2=/mnt/disk:$QCOW" "$QCOW"
    check "qcow2 stays consistent" qemu-img check "$QCOW"

    CQCOW="$WORK/compressed.qcow2"
    section "qcow2 with compressed clusters"
    qemu-img convert -f raw -O qcow2 -c "$WORK/qcow-src.img" "$CQCOW"
    OPTION="--qcow2=/mnt/disk:$CQCOW"
    IMAGE="$CQCOW"
    MNT=/mnt/disk
    check "read through a compressed cluster" check_read_file
    check "write through a compressed cluster" check_compressed_write
else
    section "qcow2 (skipped, qemu-img is missing)"
fi

if have qemu-nbd; then
    NBDIMG="$WORK/nbd.img"
    NBD_SOCKET="$WORK/nbd.sock"
    section "nbd"
    make_raw_image "$NBDIMG"
    qemu-nbd --format=raw --socket="$NBD_SOCKET" --export-name=disk \
        --persistent "$NBDIMG" >"$WORK/nbd.log" 2>&1 &
    NBD_PID=$!
    i=0
    while [ "$i" -lt 100 ]; do
        [ -S "$NBD_SOCKET" ] && break
        sleep 0.1
        i=$((i + 1))
    done
    if [ -S "$NBD_SOCKET" ]; then
        verify_rw "--nbd=/mnt/disk:nbd+unix:///disk?socket=$NBD_SOCKET" "$NBDIMG"
    else
        echo "  FAIL qemu-nbd did not start"
        fail=$((fail + 1))
        cat "$WORK/nbd.log" >&2
    fi
    kill "$NBD_PID" 2>/dev/null || true
    wait "$NBD_PID" 2>/dev/null || true
    NBD_PID=""
else
    section "nbd (skipped, qemu-nbd is missing)"
fi

if [ -n "${ISCSI_URI:-}" ] && [ -n "${ISCSI_IMAGE:-}" ]; then
    section "iscsi"
    # The raw image is created first, then the target is (re)attached to
    # it through ISCSI_ATTACH_CMD, because a target that already has the
    # file open may keep serving the previous content.
    make_raw_image "$ISCSI_IMAGE"
    if [ -n "${ISCSI_ATTACH_CMD:-}" ]; then
        sh -c "$ISCSI_ATTACH_CMD" >/dev/null 2>&1 || true
    fi
    verify_rw "--iscsi=/mnt/disk:$ISCSI_URI" "$ISCSI_IMAGE"
else
    section "iscsi (skipped, set ISCSI_URI and ISCSI_IMAGE to enable it)"
fi

echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
