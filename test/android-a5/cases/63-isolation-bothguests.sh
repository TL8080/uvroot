#!/bin/bash
# Core isolation checks in BOTH guest libc families, on the board.
#
# The full suites (61/62) run against an Alpine/musl guest.  This one repeats
# the essential vperm and read-only checks against a glibc guest (Ubuntu) as
# well, to show the isolation is a property of uvroot and not of one libc.
#
#   run from the host with:
#     ./run-as-termux.sh cases/63-isolation-bothguests.sh
#
set -u
export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export UVROOT_TMP_DIR=$PREFIX/tmp
cd "$HOME/a5" || exit 1

U=$HOME/a5/uvroot-ndk
VT=$HOME/a5/both
GP=/bin:/usr/bin:/sbin:/usr/sbin
pass=0; fail=0
ok()  { if [ "$2" = "$3" ]; then echo "  ok   $1"; pass=$((pass+1));
        else echo "  FAIL $1 (got [$2] want [$3])"; fail=$((fail+1)); fi; }
deny() { if "$@" >/dev/null 2>&1; then echo "  FAIL $1 (should have been refused)"; fail=$((fail+1));
         else echo "  ok   $1"; pass=$((pass+1)); fi; }

rm -rf "$VT"; mkdir -p "$VT"

# musl guest: fresh copy of the Alpine minirootfs
cp -a "$HOME/a5/alpine" "$VT/alpine" 2>/dev/null
# glibc guest: used in place (a full copy would be ~200 MB of small files)
UBU=$HOME/a5/ubuntu

for kind in alpine ubuntu; do
    if [ "$kind" = alpine ]; then R=$VT/alpine; LIBC=musl; else R=$UBU; LIBC=glibc; fi
    echo "############ guest: $kind ($LIBC) ############"
    # NB: /bin/sh is an absolute symlink in these rootfs images, so testing it
    # with -x would resolve against the *host* root and fail on Android.
    if [ ! -e "$R/bin/sh" ] && [ ! -L "$R/bin/sh" ]; then echo "  !! guest root unusable: $R"; continue; fi

    g()   { PATH=$GP $U -r "$R" -i 0:0 "$@" 2>/dev/null; }
    gv()  { PATH=$GP $U -r "$R" -i 0:0 --vperm "$@" 2>/dev/null; }
    as()  { local id=$1; shift; PATH=$GP $U -r "$R" -i 0:0 --vperm --vperm-id="$id" "$@" 2>/dev/null; }
    ase() { local id=$1; shift; { PATH=$GP $U -r "$R" -i 0:0 --vperm --vperm-id="$id" "$@" >/dev/null; } 2>&1; }

    mkdir -p "$R/t"
    printf 'secret\n' > "$R/t/data.txt"; chmod 644 "$R/t/data.txt"
    rm -f "$R/.uvroot-vperm"

    # --- vperm: virtual metadata must not touch host metadata -------------
    gv /bin/sh -c 'chmod 700 /t/data.txt; chown 1234:5678 /t/data.txt' >/dev/null
    ok "$kind: guest sees virtual 700 1234:5678" "$(gv /bin/stat -c '%a %u:%g' /t/data.txt)" "700 1234:5678"
    ok "$kind: host mode untouched"              "$(stat -c %a "$R/t/data.txt")" "644"
    ok "$kind: host uid untouched"               "$(stat -c %u "$R/t/data.txt")" "$(id -u)"

    # --- vperm: enforcement ----------------------------------------------
    ok "$kind: other id denied"  "$(ase 1000:1000 /bin/cat /t/data.txt | grep -c 'Permission denied')" "1"
    ok "$kind: owner allowed"    "$(as 1234:1234 /bin/cat /t/data.txt)" "secret"
    ok "$kind: virtual root allowed" "$(as 0:0 /bin/cat /t/data.txt)" "secret"

    # --- read-only --------------------------------------------------------
    mkdir -p "$R/t/locked"
    deny "$kind: --ro refuses a write"        g --ro=/t/locked /bin/sh -c 'echo x > /t/locked/f'
    ok   "$kind: --ro still allows reads"     "$(g --ro=/t/locked /bin/ls /t/locked >/dev/null 2>&1 && echo yes)" "yes"
    deny "$kind: --read-only refuses a write" g --read-only /bin/sh -c 'echo x > /t/never'
    ro_read=$(g --read-only /bin/cat /etc/os-release)
    ok   "$kind: --read-only still reads"     "$([ -n "$ro_read" ] && echo nonempty || echo empty)" "nonempty"
    deny "$kind: id0 is refused too"          g --vperm-id=0:0 --read-only /bin/sh -c 'echo x > /t/never2'
    ok   "$kind: nothing was created"         "$(test -e "$R/t/locked/f" -o -e "$R/t/never" -o -e "$R/t/never2" && echo yes || echo no)" "no"

    echo
done

echo "both-guest isolation: passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
