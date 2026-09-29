#!/bin/sh
# The netfs mount options must always be recognized, and backends that
# are not available in this build must say so instead of failing
# silently.  This test needs no server and no network.

set -e

if [ -z "$UVROOT" ]; then
    echo "UVROOT is not set" >&2
    exit 1
fi

help="$($UVROOT --help 2>&1)"

for option in --netfs --ftp --smb --nfs --iscsi --nbd --img --qcow2; do
    if ! printf '%s\n' "$help" | grep -q -- "$option"; then
        echo "option $option is missing from --help" >&2
        exit 1
    fi
done

# Every backend is recognized: it must either reach its own connection or
# image-open stage, or, when this build was made without the matching
# library, report that it is not available.  What must never happen is an
# "unknown option" style rejection.
for probe in "--nfs=/mnt/nfs:nfs://127.0.0.1:1/export" \
             "--iscsi=/mnt/lun:iscsi://127.0.0.1:1/iqn.2001-04.com.example:disk" \
             "--nbd=/mnt/nbd:nbd://127.0.0.1:1/disk" \
             "--qcow2=/mnt/img:/nonexistent/uvroot-test.qcow2"; do
    message="$($UVROOT "$probe" /bin/true 2>&1 || true)"
    case "$message" in
        *"not available in this build"*) ;;
        *"no backend"*)
            echo "backend for $probe was not recognized: $message" >&2
            exit 1
            ;;
        *) ;;
    esac
done

# FTP and SMB must not be accepted as the guest root.
message="$($UVROOT --ftp=/:ftp://example/ /bin/true 2>&1 || true)"
case "$message" in
    *"cannot be used as the guest root"*) ;;
    *)
        echo "unexpected --ftp=/ diagnostic: $message" >&2
        exit 1
        ;;
esac

# ... while a data mount is still accepted (it fails later, on the host,
# which is fine: what matters is that the mount spec is not rejected).
message="$($UVROOT -r / --ftp=/mnt/x:ftp://example/ /bin/true 2>&1 || true)"
case "$message" in
    *"cannot be used as the guest root"*)
        echo "data mount wrongly rejected: $message" >&2
        exit 1
        ;;
    *) ;;
esac

# An unknown scheme is rejected with a dedicated diagnostic.
message="$($UVROOT --netfs=/mnt/x:unknown://host/path /bin/true 2>&1 || true)"
case "$message" in
    *"no backend"*) ;;
    *)
        echo "unexpected unknown-scheme diagnostic: $message" >&2
        exit 1
        ;;
esac
