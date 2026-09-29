#!/bin/bash
# Consolidated vperm behaviour checks (local rootfs).
#
# $BINDS is intentionally split into separate arguments.
# shellcheck disable=SC2086,SC2016
set -u
UVROOT="${UVROOT:-../src/uvroot}"
R=/tmp/vperm-test/root
BINDS="-b /bin -b /usr -b /lib -b /lib64 -b /etc -b /dev"
pass=0; fail=0

vp() { "$UVROOT" -r "$R" $BINDS --vperm "$@" 2>/dev/null; }
ok() { if [ "$2" = "$3" ]; then echo "  ok   $1"; pass=$((pass+1));
       else echo "  FAIL $1 (got [$2] want [$3])"; fail=$((fail+1)); fi; }

rm -rf "$R"; mkdir -p "$R"
printf 'secret\n' > "$R/data.txt"; chmod 644 "$R/data.txt"

echo "== metadata =="
vp /bin/sh -c 'chmod 700 /data.txt' >/dev/null
ok "host mode untouched" "$(stat -c %a "$R/data.txt")" "644"
ok "guest sees virtual mode" "$(vp /usr/bin/stat -c %a /data.txt)" "700"
vp /bin/sh -c 'chown 1234:5678 /data.txt' >/dev/null
ok "guest sees virtual owner" "$(vp /usr/bin/stat -c %u:%g /data.txt)" "1234:5678"
ok "host owner untouched" "$(stat -c %u:%g "$R/data.txt")" "$(id -u):$(id -g)"

echo "== access control =="
ok "deny other"   "$({ "$UVROOT" -r "$R" $BINDS --vperm --vperm-id=1000:1000 /bin/cat /data.txt >/dev/null; } 2>&1 | grep -c '权限\|denied')" "1"
ok "allow owner"  "$("$UVROOT" -r "$R" $BINDS --vperm --vperm-id=1234:1234 /bin/cat /data.txt 2>/dev/null)" "secret"
ok "allow root"   "$("$UVROOT" -r "$R" $BINDS --vperm --vperm-id=0:0 /bin/cat /data.txt 2>/dev/null)" "secret"

echo "== pass-through =="
printf 'pub\n' > "$R/pub.txt"; chmod 666 "$R/pub.txt"
ok "no entry uses host" "$("$UVROOT" -r "$R" $BINDS --vperm --vperm-id=1000:1000 /bin/cat /pub.txt 2>/dev/null)" "pub"

echo "== ancestor x and parent w =="
vp /bin/sh -c 'mkdir /sub; echo inner > /sub/f.txt; chmod 700 /sub; chown 1234:1234 /sub' >/dev/null
ok "ancestor x denies"   "$({ "$UVROOT" -r "$R" $BINDS --vperm --vperm-id=1000:1000 /bin/cat /sub/f.txt >/dev/null; } 2>&1 | grep -c '权限\|denied')" "1"
ok "ancestor x allows"   "$("$UVROOT" -r "$R" $BINDS --vperm --vperm-id=1234:1234 /bin/cat /sub/f.txt 2>/dev/null)" "inner"
ok "parent w denies"     "$({ "$UVROOT" -r "$R" $BINDS --vperm --vperm-id=1000:1000 /bin/sh -c 'echo x > /sub/n.txt' >/dev/null; } 2>&1 | grep -c '权限\|denied')" "1"
ok "parent w allows"     "$("$UVROOT" -r "$R" $BINDS --vperm --vperm-id=1234:1234 /bin/sh -c 'echo x > /sub/n.txt && echo yes' 2>/dev/null)" "yes"

echo "== database protection =="
ok "db unreadable"      "$({ "$UVROOT" -r "$R" $BINDS --vperm /bin/cat /.uvroot-vperm >/dev/null; } 2>&1 | grep -c '权限\|denied')" "1"
ok "db hidden in ls -a" "$(vp /bin/ls -a / | grep -c '^\.uvroot-vperm$')" "0"
ok "db still on host"   "$(test -f "$R/.uvroot-vperm" && echo yes)" "yes"

echo "== maintenance =="
vp /bin/mv /sub /sub2 >/dev/null
ok "rename relocates tree" "$(grep -c 'sub2/f.txt' "$R/.uvroot-vperm")" "1"
ok "rename removes old"    "$(grep -c 'sub/f.txt' "$R/.uvroot-vperm")" "0"
vp /bin/rm -f /data.txt >/dev/null
ok "unlink keeps entry"    "$(grep -c 'data.txt' "$R/.uvroot-vperm")" "1"

echo "== sticky entries: kept while gone, refreshed when recreated =="
vp /bin/sh -c 'chown 1234:5678 /data.txt; chmod 640 /data.txt' >/dev/null
vp /bin/rm -f /data.txt >/dev/null
ok "entry survives unlink" "$(grep -c 'data.txt' "$R/.uvroot-vperm")" "1"
ok "no .bak written"       "$(test -f "$R/.uvroot-vperm.bak" && echo yes || echo no)" "no"
vp /bin/sh -c 'umask 0; echo new > /data.txt' >/dev/null
ok "sticky owner kept"     "$(vp /usr/bin/stat -c %u:%g /data.txt)" "1234:5678"
ok "mode from creation"    "$(vp /usr/bin/stat -c %a /data.txt)" "666"
ok "entry refreshed"       "$(grep -P '\tdata\.txt$' "$R/.uvroot-vperm" | awk '{print $1, $2, $3}')" "100666 1234 5678"

echo "== damaged database stops uvroot =="
cp "$R/.uvroot-vperm" /tmp/vperm-test/keep.db
printf 'this is not an entry\n' > "$R/.uvroot-vperm"
if "$UVROOT" -r "$R" $BINDS --vperm /bin/true >/dev/null 2>&1; then
    ok "damaged db refused" "started" "refused"
else
    ok "damaged db refused" "refused" "refused"
fi
cp /tmp/vperm-test/keep.db "$R/.uvroot-vperm"

echo "== legacy database is taken over =="
rm -f "$R/.uvroot-vperm"; printf '700 0 0\tdata.txt\n' > "$R/.proot-vperm"
ok "upstream name loaded"  "$(vp /usr/bin/stat -c %u /data.txt)" "0"
ok "upstream name renamed" "$(test -f "$R/.uvroot-vperm" && test ! -e "$R/.proot-vperm" && echo yes)" "yes"

echo "== virtual su/sudo (switchable mapping directory) =="
MP=/tmp/vperm-test/map
rm -rf "$MP"; mkdir -p "$MP"
printf 'root:0:0:/bin/sh\nuser1:1000:1000:/bin/sh\nuser2:2000:2000:/bin/sh\n' \
    > "$MP/users.conf"
printf '700 1000 1000\tdata.txt\n' > "$R/.uvroot-vperm"
printf 'protected\n' > "$R/data.txt"

vp_su() { "$UVROOT" -r "$R" $BINDS --vperm --vperm-map="$MP" --vperm-id=2000:2000 "$@" 2>/dev/null; }
vp_err() { { "$UVROOT" -r "$R" $BINDS --vperm --vperm-map="$MP" --vperm-id=2000:2000 "$@" >/dev/null; } 2>&1; }

ok "su switches to user1" "$(vp_su /usr/bin/su user1 -c 'id -u')" "1000"
ok "su user1 reads own"   "$(vp_su /usr/bin/su user1 -c 'cat /data.txt')" "protected"
ok "su user2 denied"      "$(vp_err /usr/bin/su user2 -c 'cat /data.txt' | grep -c '权限\|denied')" "1"
ok "su to root denied"    "$(vp_err /usr/bin/su root -c 'id -u' | grep -c 'may not switch')" "1"
ok "sudo to root denied"  "$(vp_err /usr/bin/sudo /usr/bin/id -u | grep -c 'may not switch')" "1"
ok "map dir is used"      "$(test -x "$MP/shim/su" && test -x "$MP/shim/sudo" && echo yes)" "yes"

echo "== identity hardening =="
# Direct use of the magic switch path is not a way around the policy.
ok "magic path to root denied" \
    "$(vp_err /bin/sh -c 'exec /.uvroot-vperm-switch 0 /usr/bin/id -u' | grep -c 'may not switch')" "1"

if command -v python3 >/dev/null 2>&1; then
    PY='import os,sys; os.setgid(int(sys.argv[1])); os.setuid(int(sys.argv[1])); print(os.getuid())'
    # A non-root id may only pass its own value back to the setuid family.
    ok "setuid(0) denied"     "$(vp_su /usr/bin/python3 -c "$PY" 0)" ""
    ok "setuid(self) allowed" "$(vp_su /usr/bin/python3 -c "$PY" 2000)" "2000"
    # ... while the virtual root keeps switching freely.
    ok "root switches id" \
        "$("$UVROOT" -r "$R" $BINDS --vperm --vperm-id=0 /usr/bin/python3 -c "$PY" 1000 2>/dev/null)" "1000"

    echo "== process permissions =="
    # Another id must not be able to kill this id's processes.
    ok "cross-id kill denied" "$("$UVROOT" -r "$R" $BINDS --vperm --vperm-id=0 /bin/sh -c '
        sleep 300 & P=$!
        /usr/bin/python3 -c "import os,sys
os.setgid(1000); os.setuid(1000); os.kill(int(sys.argv[1]), 9)" $P 2>/dev/null
        if /bin/kill -0 $P 2>/dev/null; then echo alive; fi
        /bin/kill -9 $P 2>/dev/null' 2>/dev/null)" "alive"
    # Same id keeps working normally.
    ok "same-id kill allowed" "$("$UVROOT" -r "$R" $BINDS --vperm --vperm-id=1000 /bin/sh -c '
        sleep 300 & P=$!
        /bin/kill -9 $P && echo killed' 2>/dev/null)" "killed"
fi

# The virtual root may signal anything.
ok "root kill allowed"    "$("$UVROOT" -r "$R" $BINDS --vperm --vperm-id=0 /bin/sh -c '
    sleep 300 & P=$!
    /bin/kill -9 $P && echo killed' 2>/dev/null)" "killed"

echo "== /etc/passwd read-write, shim protection, --vperm-nosu =="
vp2() { "$UVROOT" -r "$R" $BINDS --vperm --vperm-map="$MP" --vperm-id=2000 "$@" 2>/dev/null; }
vp2e() { { "$UVROOT" -r "$R" $BINDS --vperm --vperm-map="$MP" --vperm-id=2000 "$@" >/dev/null; } 2>&1; }

ok "passwd bound"      "$(vp2 /bin/cat /etc/passwd | grep -c '^root:')" "1"
vp2 /bin/sh -c 'echo user9:x:900:900:v:/root:/bin/sh >> /etc/passwd' >/dev/null
ok "passwd writable"   "$(grep -c '^user9:' "$MP/passwd")" "1"
ok "new user usable"   "$(vp2 /usr/bin/su user9 -c 'id -u')" "900"

shim_before="$(sha256sum "$MP/shim/su" | cut -c1-16)"
vp2e /bin/rm -f /bin/su >/dev/null || true
vp2e /bin/sh -c 'echo x > /bin/su' >/dev/null || true
ok "shim not deletable" "$(test -x "$MP/shim/su" && echo yes)" "yes"
ok "shim not writable"  "$(test "$shim_before" = "$(sha256sum "$MP/shim/su" | cut -c1-16)" && echo yes)" "yes"
ok "shim chmod denied"  "$(vp2e /bin/chmod 777 /bin/su | grep -c '不允许\|not permitted')" "1"

MP2=/tmp/vperm-test/map2
rm -rf "$MP2"; mkdir -p "$MP2"; printf 'user1:1000:1000:/bin/sh\n' > "$MP2/users.conf"
"$UVROOT" -r "$R" $BINDS --vperm-nosu --vperm-map="$MP2" --vperm-id=1000 \
    /usr/bin/id -u >/dev/null 2>&1 || true
ok "nosu: no shims"     "$(test -e "$MP2/shim/su" && echo yes || echo no)" "no"
ok "nosu: passwd bound" "$("$UVROOT" -r "$R" $BINDS --vperm-nosu --vperm-map="$MP2" /bin/cat /etc/passwd 2>/dev/null | grep -c '^user1:')" "1"
ok "bare uid accepted"  "$("$UVROOT" -r "$R" $BINDS --vperm --vperm-id=1000 /usr/bin/id -u 2>/dev/null)" "1000"

echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
