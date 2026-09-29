#!/bin/bash
# vperm (virtual users) on the Android board, Alpine/musl guest.
#
# Device-adapted version of test/test-vperm.sh: the host suite binds the
# host's /bin,/usr,/lib,/lib64,/etc into an empty root; here the guest is a
# copy of the Alpine minirootfs, so the toolchain comes from the guest and
# the guest PATH must be set explicitly (the inherited Termux PATH means
# nothing inside the container).
#
#   run from the host with:
#     ./run-as-termux.sh cases/61-vperm-device.sh
#
set -u
export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export UVROOT_TMP_DIR=$PREFIX/tmp
export ZIG_GLOBAL_CACHE_DIR=$HOME/.cache/zig
cd "$HOME/a5" || exit 1

U=$HOME/a5/uvroot-ndk          # NDK-built uvroot on the device
SRC=$HOME/a5/alpine            # pristine Alpine minirootfs
VT=$HOME/a5/vt                 # scratch
R=$VT/root                     # guest root for this run
MAP=$VT/map                    # vperm mapping directory
MAP2=$VT/map2
GP=/bin:/usr/bin:/sbin:/usr/sbin

pass=0; fail=0
ok()  { if [ "$2" = "$3" ]; then echo "  ok   $1"; pass=$((pass+1));
        else echo "  FAIL $1 (got [$2] want [$3])"; fail=$((fail+1)); fi; }
deny() { # $1 name, rest: command that must fail
  if "$@" >/dev/null 2>&1; then echo "  FAIL $1 (should have been refused)"; fail=$((fail+1));
  else echo "  ok   $1"; pass=$((pass+1)); fi; }

# ---------------------------------------------------------------- setup ----
rm -rf "$VT"; mkdir -p "$R" "$MAP" "$MAP2"
cp -a "$SRC"/. "$R"/ 2>/dev/null
mkdir -p "$R/t"
printf 'secret\n' > "$R/t/data.txt"; chmod 644 "$R/t/data.txt"

cat > "$VT/setid.c" <<'EOF'
/* setid <uid> [pid]  - drop to uid, optionally kill pid, print resulting uid */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    int uid = atoi(argv[1]);
    if (setgid((gid_t)uid) != 0) return 1;
    if (setuid((uid_t)uid) != 0) return 1;   /* non-root may not switch away */
    if (argc >= 3) {
        if (kill(atoi(argv[2]), 9) != 0) return 3;
        printf("killed\n");
        return 0;
    }
    printf("%d\n", (int)getuid());
    return 0;
}
EOF
zig cc -target aarch64-linux-musl -static -O1 "$VT/setid.c" -o "$R/t/setid" 2>/dev/null && chmod 755 "$R/t/setid"
if [ ! -x "$R/t/setid" ]; then echo "  !! setid helper missing - setuid/kill tests will fail"; fi

# uvroot under test: no -i, exactly like the upstream host suite.
vp()  { PATH=$GP $U -r "$R" --vperm "$@" 2>/dev/null; }
vpe() { { PATH=$GP $U -r "$R" --vperm "$@" >/dev/null; } 2>&1; }
as()  { local id=$1; shift; PATH=$GP $U -r "$R" --vperm --vperm-id="$id" "$@" 2>/dev/null; }
ase() { local id=$1; shift; { PATH=$GP $U -r "$R" --vperm --vperm-id="$id" "$@" >/dev/null; } 2>&1; }

echo "############ vperm on Android/arm64 (guest: Alpine/musl) ############"
echo "uvroot: $($U --version 2>&1 | head -1 | tr -d '\n' | tail -c 40)"
echo "guest : $($U -r "$R" /bin/cat /etc/alpine-release 2>/dev/null | tr -d '\n')"
echo

echo "== metadata =="
vp /bin/sh -c 'chmod 700 /t/data.txt' >/dev/null
ok "host mode untouched"      "$(stat -c %a "$R/t/data.txt")" "644"
ok "guest sees virtual mode"  "$(vp /bin/stat -c %a /t/data.txt)" "700"
vp /bin/sh -c 'chown 1234:5678 /t/data.txt' >/dev/null
ok "guest sees virtual owner" "$(vp /bin/stat -c %u:%g /t/data.txt)" "1234:5678"
ok "host owner untouched"     "$(stat -c %u:%g "$R/t/data.txt")" "$(id -u):$(id -g)"

echo "== access control =="
ok "deny other"  "$(ase 1000:1000 /bin/cat /t/data.txt | grep -c 'Permission denied')" "1"
ok "allow owner" "$(as 1234:1234 /bin/cat /t/data.txt)" "secret"
ok "allow root"  "$(as 0:0 /bin/cat /t/data.txt)" "secret"

echo "== pass-through (no entry => host metadata) =="
printf 'pub\n' > "$R/t/pub.txt"; chmod 666 "$R/t/pub.txt"
ok "no entry uses host" "$(as 1000:1000 /bin/cat /t/pub.txt)" "pub"

echo "== ancestor x and parent w =="
vp /bin/sh -c 'mkdir /t/sub; echo inner > /t/sub/f.txt; chmod 700 /t/sub; chown 1234:1234 /t/sub' >/dev/null
ok "ancestor x denies" "$(ase 1000:1000 /bin/cat /t/sub/f.txt | grep -c 'Permission denied')" "1"
ok "ancestor x allows" "$(as 1234:1234 /bin/cat /t/sub/f.txt)" "inner"
ok "parent w denies"   "$(ase 1000:1000 /bin/sh -c 'echo x > /t/sub/n.txt' | grep -c 'Permission denied')" "1"
ok "parent w allows"   "$(as 1234:1234 /bin/sh -c 'echo x > /t/sub/n.txt && echo yes')" "yes"

echo "== database protection =="
ok "db unreadable"     "$(vpe /bin/cat /.uvroot-vperm | grep -c 'Permission denied')" "1"
ok "db hidden in ls -a" "$(vp /bin/ls -a / | grep -c '^\.uvroot-vperm$')" "0"
ok "db still on host"  "$(test -f "$R/.uvroot-vperm" && echo yes)" "yes"
ok "db owner is app"   "$(stat -c %u "$R/.uvroot-vperm")" "$(id -u)"

echo "== maintenance =="
vp /bin/mv /t/sub /t/sub2 >/dev/null
ok "rename relocates tree" "$(grep -c 'sub2/f.txt' "$R/.uvroot-vperm")" "1"
ok "rename removes old"    "$(grep -c 'sub/f.txt' "$R/.uvroot-vperm")" "0"
vp /bin/rm -f /t/data.txt >/dev/null
ok "unlink keeps entry"    "$(grep -c 'data.txt' "$R/.uvroot-vperm")" "1"

echo "== sticky entries: kept while gone, refreshed when recreated =="
vp /bin/sh -c 'chown 1234:5678 /t/data.txt; chmod 640 /t/data.txt' >/dev/null
vp /bin/rm -f /t/data.txt >/dev/null
ok "entry survives unlink" "$(grep -c 'data.txt' "$R/.uvroot-vperm")" "1"
ok "no backup written"     "$(test -f "$R/.uvroot-vperm.bak" && echo yes || echo no)" "no"
vp /bin/sh -c 'umask 0; echo new > /t/data.txt' >/dev/null
ok "sticky owner kept"     "$(vp /bin/stat -c %u:%g /t/data.txt)" "1234:5678"
ok "mode from creation"    "$(vp /bin/stat -c %a /t/data.txt)" "666"
ok "entry refreshed"       "$(grep -P '\tt/data\.txt$' "$R/.uvroot-vperm" | awk '{print $1, $2, $3}')" "100666 1234 5678"

echo "== damaged database stops uvroot =="
cp "$R/.uvroot-vperm" "$VT/keep.db"
printf 'this is not an entry\n' > "$R/.uvroot-vperm"
if vp /bin/true >/dev/null 2>&1; then
    ok "damaged db refused" "started" "refused"
else
    ok "damaged db refused" "refused" "refused"
fi
cp "$VT/keep.db" "$R/.uvroot-vperm"

echo "== legacy database is taken over =="
rm -f "$R/.uvroot-vperm"; printf '700 0 0\tt/data.txt\n' > "$R/.proot-vperm"
ok "upstream name loaded"  "$(vp /bin/stat -c %u /t/data.txt)" "0"
ok "upstream name renamed" "$(test -f "$R/.uvroot-vperm" && test ! -e "$R/.proot-vperm" && echo yes)" "yes"
rm -f "$R/.uvroot-vperm"; printf '700 0 0\tt/data.txt\n' > "$R/.nvroot.vperm"
ok "interim name loaded"   "$(vp /bin/stat -c %u /t/data.txt)" "0"
ok "interim name renamed"  "$(test -f "$R/.uvroot-vperm" && test ! -e "$R/.nvroot.vperm" && echo yes)" "yes"

echo "== virtual su/sudo (mapping directory) =="
printf 'root:0:0:/bin/sh\nuser1:1000:1000:/bin/sh\nuser2:2000:2000:/bin/sh\n' > "$MAP/users.conf"
printf '700 1000 1000\tt/data.txt\n' > "$R/.uvroot-vperm"
printf 'protected\n' > "$R/t/data.txt"

su1()  { PATH=$GP $U -r "$R" --vperm --vperm-map="$MAP" --vperm-id=2000:2000 /bin/su "$@" 2>/dev/null; }
su1e() { { PATH=$GP $U -r "$R" --vperm --vperm-map="$MAP" --vperm-id=2000:2000 /bin/su "$@" >/dev/null; } 2>&1; }

ok "su switches to user1" "$(su1 user1 -c 'id -u')" "1000"
ok "su user1 reads own"   "$(su1 user1 -c 'cat /t/data.txt')" "protected"
ok "su user2 denied"      "$(su1e user2 -c 'cat /t/data.txt' | grep -c 'Permission denied')" "1"
ok "su to root denied"    "$(su1e root -c 'id -u' | grep -c 'may not switch')" "1"
ok "map dir is used"      "$(test -x "$MAP/shim/su" && echo yes)" "yes"
ok "shim bound as /bin/su" "$(PATH=$GP $U -r "$R" --vperm --vperm-map="$MAP" --vperm-id=2000:2000 /bin/sh -c 'cat /bin/su' 2>/dev/null | grep -c 'Virtual su shim generated by uvroot')" "1"

echo "== identity hardening =="
ok "magic path to root denied" \
   "$({ PATH=$GP $U -r "$R" --vperm --vperm-map="$MAP" --vperm-id=2000:2000 /bin/sh -c 'exec /.uvroot-vperm-switch 0 /bin/id -u' >/dev/null; } 2>&1 | grep -c 'may not switch')" "1"

echo "== setuid family =="
ok "setuid(0) denied"     "$(PATH=$GP $U -r "$R" --vperm --vperm-map="$MAP" --vperm-id=2000:2000 /t/setid 0 2>/dev/null)" ""
ok "setuid(self) allowed" "$(PATH=$GP $U -r "$R" --vperm --vperm-map="$MAP" --vperm-id=2000:2000 /t/setid 2000 2>/dev/null)" "2000"
ok "root switches id"     "$(PATH=$GP $U -r "$R" --vperm --vperm-map="$MAP" --vperm-id=0 /t/setid 1000 2>/dev/null)" "1000"

echo "== process permissions =="
ok "cross-id kill denied" "$(PATH=$GP $U -r "$R" --vperm --vperm-id=0 /bin/sh -c '
    /bin/sleep 300 >/dev/null 2>&1 & P=$!
    /t/setid 1000 $P 2>/dev/null
    if /bin/kill -0 $P 2>/dev/null; then echo alive; fi
    /bin/kill -9 $P 2>/dev/null' 2>/dev/null)" "alive"
ok "same-id kill allowed" "$(PATH=$GP $U -r "$R" --vperm --vperm-id=1000 /bin/sh -c '
    /bin/sleep 300 >/dev/null 2>&1 & P=$!
    /bin/kill -9 $P && echo killed' 2>/dev/null)" "killed"
ok "root kill allowed"    "$(PATH=$GP $U -r "$R" --vperm --vperm-id=0 /bin/sh -c '
    /bin/sleep 300 >/dev/null 2>&1 & P=$!
    /bin/kill -9 $P && echo killed' 2>/dev/null)" "killed"

echo "== /etc/passwd read-write and shim protection =="
p2()  { PATH=$GP $U -r "$R" --vperm --vperm-map="$MAP" --vperm-id=2000 "$@" 2>/dev/null; }
p2e() { { PATH=$GP $U -r "$R" --vperm --vperm-map="$MAP" --vperm-id=2000 "$@" >/dev/null; } 2>&1; }

ok "passwd bound"    "$(p2 /bin/cat /etc/passwd | grep -c '^root:')" "1"
p2 /bin/sh -c 'echo user9:x:900:900:v:/root:/bin/sh >> /etc/passwd' >/dev/null
ok "passwd writable" "$(grep -c '^user9:' "$MAP/passwd")" "1"
ok "new user usable" "$(p2 /bin/su user9 -c 'id -u')" "900"

shim_before="$(sha256sum "$MAP/shim/su" | cut -c1-16)"
deny "shim not deletable" p2e /bin/rm -f /bin/su
deny "shim not writable"  p2e /bin/sh -c 'echo x > /bin/su'
ok "shim unchanged"       "$(test "$shim_before" = "$(sha256sum "$MAP/shim/su" | cut -c1-16)" && echo yes)" "yes"

echo "== --vperm-nosu =="
printf 'user1:1000:1000:/bin/sh\n' > "$MAP2/users.conf"
PATH=$GP $U -r "$R" --vperm-nosu --vperm-map="$MAP2" --vperm-id=1000 /bin/id -u >/dev/null 2>&1 || true
ok "nosu: no shims"     "$(test -e "$MAP2/shim/su" && echo yes || echo no)" "no"
ok "nosu: passwd bound" "$(PATH=$GP $U -r "$R" --vperm-nosu --vperm-map="$MAP2" /bin/cat /etc/passwd 2>/dev/null | grep -c '^user1:')" "1"
ok "bare uid accepted"  "$(PATH=$GP $U -r "$R" --vperm --vperm-id=1000 /usr/bin/id -u 2>/dev/null)" "1000"

echo
echo "vperm: passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
