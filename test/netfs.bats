#!/usr/bin/env bats
#
# netfs: user-space mounts for FTP/FTPS/SFTP and SMB.
#
# The round-trip test needs pyftpdlib (pip install pyftpdlib); it is
# skipped when the module is not available so the suite stays usable
# offline.

load helper

setup() {
    NETFS_TMP="$(mktemp -d)"
    NETFS_MNT="$NETFS_TMP/mnt"
    mkdir -p "$NETFS_TMP/remote"
    printf 'hello from netfs\n' > "$NETFS_TMP/remote/hello.txt"
    FTP_PID=""
    NBD_PID=""
}

teardown() {
    if [ -n "$FTP_PID" ]; then
        kill "$FTP_PID" 2>/dev/null || true
        wait "$FTP_PID" 2>/dev/null || true
    fi
    if [ -n "$NBD_PID" ]; then
        kill "$NBD_PID" 2>/dev/null || true
        wait "$NBD_PID" 2>/dev/null || true
    fi
    rm -rf "$NETFS_TMP"
}

wait_for_port() {
    local port="$1"

    for _ in $(seq 1 100); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

@test "netfs advertises its mount options" {
    run uvroot --help
    [ "$status" -eq 0 ]
    [[ "$output" == *"--netfs"* ]]
    [[ "$output" == *"--ftp"* ]]
    [[ "$output" == *"--smb"* ]]
    [[ "$output" == *"--nfs"* ]]
    [[ "$output" == *"--qcow2"* ]]
}

# NFS needs an exported directory on a reachable server, which the suite
# cannot set up portably, so it is exercised only when the environment
# points at one.  See test/test-nfs.sh for a helper that starts a
# user-space server.  When the build was made without libnfs, the option
# must report that instead of being rejected.
@test "netfs mounts an NFS export read-write" {
    if [ -z "${NFS_TEST_URI:-}" ] || [ -z "${NFS_TEST_DIR:-}" ]; then
        run uvroot --nfs=/mnt/nfs:nfs://127.0.0.1:1/export /bin/true
        [ "$status" -ne 0 ]
        [[ "$output" != *"no backend"* ]]
        skip "set NFS_TEST_URI and NFS_TEST_DIR to run the NFS round trip"
    fi

    printf 'hello from netfs\n' > "$NFS_TEST_DIR/hello.txt"
    mkdir -p "$NFS_TEST_DIR/sub"
    printf 'nested file\n' > "$NFS_TEST_DIR/sub/nested.txt"

    run uvroot -r / "--nfs=$NETFS_MNT:$NFS_TEST_URI" /bin/cat "$NETFS_MNT/hello.txt"
    [ "$status" -eq 0 ]
    [[ "$output" == *"hello from netfs"* ]]

    run uvroot -r / "--nfs=$NETFS_MNT:$NFS_TEST_URI" /bin/sh -c \
        "echo written > '$NETFS_MNT/new.txt' && mkdir '$NETFS_MNT/dir'"
    [ "$status" -eq 0 ]
    [ "$(cat "$NFS_TEST_DIR/new.txt")" = "written" ]
    [ -d "$NFS_TEST_DIR/dir" ]

    run uvroot -r / "--nfs=$NETFS_MNT:$NFS_TEST_URI" /bin/sh -c \
        "mv '$NETFS_MNT/new.txt' '$NETFS_MNT/renamed.txt' &&
         rm -f '$NETFS_MNT/hello.txt' && rmdir '$NETFS_MNT/dir'"
    [ "$status" -eq 0 ]
    [ -f "$NFS_TEST_DIR/renamed.txt" ]
    [ ! -e "$NFS_TEST_DIR/hello.txt" ]

    # Symbolic links found on the server must be followed through.
    ln -sf hello.txt "$NFS_TEST_DIR/hostlink"
    run uvroot -r / "--nfs=$NETFS_MNT:$NFS_TEST_URI" /bin/readlink "$NETFS_MNT/hostlink"
    [ "$status" -eq 0 ]
    [[ "$output" == *"hello.txt"* ]]
}

@test "netfs mounts a qcow2 image read-write" {
    check_if_command_exists qemu-img
    check_if_command_exists mke2fs
    check_if_command_exists debugfs

    local raw="$NETFS_TMP/disk.raw"
    local image="$NETFS_TMP/disk.qcow2"

    dd if=/dev/zero of="$raw" bs=1M count=16 status=none
    mke2fs -q -t ext4 -F -d "$NETFS_TMP/remote" "$raw"
    qemu-img convert -f raw -O qcow2 "$raw" "$image"

    run uvroot -r / "--qcow2=$NETFS_MNT:$image" /bin/cat "$NETFS_MNT/hello.txt"
    [ "$status" -eq 0 ]
    [[ "$output" == *"hello from netfs"* ]]

    run uvroot -r / "--qcow2=$NETFS_MNT:$image" /bin/sh -c \
        "echo written > '$NETFS_MNT/new.txt'"
    [ "$status" -eq 0 ]

    # debugfs cannot read a QCOW2 container directly, so unpack it first.
    qemu-img convert -f qcow2 -O raw "$image" "$NETFS_TMP/check.raw"
    run debugfs -R 'cat /new.txt' "$NETFS_TMP/check.raw"
    [[ "$output" == *"written"* ]]

    # The image must stay a valid QCOW2 container.
    run qemu-img check "$image"
    [ "$status" -eq 0 ]
}

@test "netfs replaces an existing destination on a qcow2 image" {
    check_if_command_exists qemu-img
    check_if_command_exists mke2fs
    check_if_command_exists debugfs
    check_if_command_exists e2fsck

    local raw="$NETFS_TMP/replace.raw"
    local image="$NETFS_TMP/replace.qcow2"
    local check="$NETFS_TMP/replace-check.raw"

    dd if=/dev/zero of="$raw" bs=1M count=16 status=none
    mkdir -p "$NETFS_TMP/seed"
    printf 'OLD\n' > "$NETFS_TMP/seed/victim.txt"
    mke2fs -q -t ext4 -F -d "$NETFS_TMP/seed" "$raw"
    qemu-img convert -f raw -O qcow2 "$raw" "$image"

    # rename(2) replaces an existing destination.  Appending a second
    # directory entry with the same name would shadow the new content and
    # leave the filesystem with duplicate names; unlinking a fast symlink
    # must not read its inline target as block numbers either.
    run uvroot -r / "--qcow2=$NETFS_MNT:$image" /bin/sh -c \
        "echo NEW > '$NETFS_MNT/source.txt' &&
         mv '$NETFS_MNT/source.txt' '$NETFS_MNT/victim.txt' &&
         ln -s victim.txt '$NETFS_MNT/link.old' &&
         ln -s other '$NETFS_MNT/link.new' &&
         mv '$NETFS_MNT/link.old' '$NETFS_MNT/link.new' &&
         rm -f '$NETFS_MNT/link.new'"
    [ "$status" -eq 0 ]

    qemu-img convert -f qcow2 -O raw "$image" "$check"

    run debugfs -R 'cat /victim.txt' "$check"
    [ "$status" -eq 0 ]
    [[ "$output" == *"NEW"* ]]

    # Exactly one entry may carry the destination name.
    run debugfs -R 'ls -l /' "$check"
    [ "$status" -eq 0 ]
    [ "$(grep -c 'victim.txt' <<<"$output")" -eq 1 ]

    run e2fsck -fn "$check"
    [ "$status" -eq 0 ]
}

@test "netfs mounts an NBD export read-write" {
    check_if_command_exists qemu-nbd
    check_if_command_exists mke2fs
    check_if_command_exists debugfs

    local raw="$NETFS_TMP/nbd.raw"
    local socket="$NETFS_TMP/nbd.sock"

    dd if=/dev/zero of="$raw" bs=1M count=16 status=none
    mke2fs -q -t ext4 -F -d "$NETFS_TMP/remote" "$raw"

    qemu-nbd --format=raw --socket="$socket" --export-name=disk \
        --persistent "$raw" >"$NETFS_TMP/nbd.log" 2>&1 &
    NBD_PID=$!
    for _ in $(seq 1 100); do
        [ -S "$socket" ] && break
        sleep 0.1
    done
    [ -S "$socket" ] || {
        cat "$NETFS_TMP/nbd.log" >&2
        false
    }

    run uvroot -r / "--nbd=$NETFS_MNT:nbd+unix:///disk?socket=$socket" \
        /bin/cat "$NETFS_MNT/hello.txt"
    [ "$status" -eq 0 ]
    [[ "$output" == *"hello from netfs"* ]]

    run uvroot -r / "--nbd=$NETFS_MNT:nbd+unix:///disk?socket=$socket" \
        /bin/sh -c "echo written > '$NETFS_MNT/new.txt'"
    [ "$status" -eq 0 ]
    run debugfs -R 'cat /new.txt' "$raw"
    [[ "$output" == *"written"* ]]
}

@test "netfs mounts an FTP share read-write" {
    python3 -c 'import pyftpdlib' 2>/dev/null \
        || skip "pyftpdlib is not installed"

    local port=21210

    python3 -m pyftpdlib -i 127.0.0.1 -p "$port" -w \
        -d "$NETFS_TMP/remote" -u user -P pass \
        >"$NETFS_TMP/ftp.log" 2>&1 &
    FTP_PID=$!
    wait_for_port "$port" || {
        cat "$NETFS_TMP/ftp.log" >&2
        false
    }

    local uri="ftp://user:pass@127.0.0.1:$port/"
    local mount="$NETFS_MNT:$uri"

    # Read-only path: directory listing and file content.  Bats' run()
    # merges stderr, where uvroot reports the mount, so match on content.
    run uvroot -r / "--ftp=$mount" /bin/cat "$NETFS_MNT/hello.txt"
    [ "$status" -eq 0 ]
    [[ "$output" == *"hello from netfs"* ]]

    # Write path: create, write back, and read the result from the server.
    run uvroot -r / "--ftp=$mount" /bin/sh -c \
        "echo written > '$NETFS_MNT/new.txt' && mkdir '$NETFS_MNT/dir'"
    [ "$status" -eq 0 ]
    [ "$(cat "$NETFS_TMP/remote/new.txt")" = "written" ]
    [ -d "$NETFS_TMP/remote/dir" ]

    # Namespace operations: rename and remove.
    run uvroot -r / "--ftp=$mount" /bin/sh -c \
        "mv '$NETFS_MNT/new.txt' '$NETFS_MNT/renamed.txt' && rm -f '$NETFS_MNT/hello.txt'"
    [ "$status" -eq 0 ]
    [ -f "$NETFS_TMP/remote/renamed.txt" ]
    [ ! -e "$NETFS_TMP/remote/hello.txt" ]
}
