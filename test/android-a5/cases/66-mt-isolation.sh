#!/bin/bash
# shellcheck disable=SC2012,SC2001
# Does the uvroot isolation actually do anything?  Let MT Manager answer.
#
# MT Manager (bin.mt.plus) is a *file manager*: its whole job is to browse
# /sdcard, /storage, other apps' data.  So we run the same probe, driving MT
# Manager's own native code (libmt1/libmt3), twice:
#
#   baseline  -- as an ordinary Termux process, no uvroot
#   isolated  -- inside `run.sh`'s uvroot root
#
# and diff the two.  Everything here runs as uid 10117; no root.
#
#   host:  ./app-isolation/build-and-push.sh
#          ./run-as-termux.sh cases/66-mt-isolation.sh
#
set -u
export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets:/system/bin
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export UVROOT_TMP_DIR=$PREFIX/tmp
cd "$HOME/a5" || exit 1

PKG=${PKG:-bin.mt.plus}
U=$HOME/a5/uvroot-ndk
BASE=$HOME/a5/mtiso
ROOT=$BASE/root
SHADOW=$BASE/shadow
HARNESS=$BASE/harness
LIBS=$HARNESS/mtlibs
AI=$HOME/a5/app-isolation
[ -x "$U" ] || { echo "!! no uvroot at $U"; exit 1; }

pass=0; fail=0
ok()   { if [ "$2" = "$3" ]; then echo "  ok   $1"; pass=$((pass+1));
         else echo "  FAIL $1 (got [$2] want [$3])"; fail=$((fail+1)); fi; }
deny() { if "$@" >/dev/null 2>&1; then echo "  FAIL $1 (should have been refused)"; fail=$((fail+1));
         else echo "  ok   $1"; pass=$((pass+1)); fi; }
getv() { printf '%s\n' "$1" | grep -F -m1 "PROBE: $2=" | cut -d= -f2-; }

# ---------------------------------------------------------------------------
echo "###### 0. the subject ######"
APK=$(/system/bin/pm path "$PKG" 2>/dev/null | sed -n 's/^package://p' | head -1)
[ -n "$APK" ] || { echo "!! $PKG is not installed"; exit 1; }
APP_CODE=$(dirname "$APK")
echo "  pkg      = $PKG  (MT Manager)"
echo "  code dir = $APP_CODE"
echo "  apk size = $(stat -c %s "$APK") bytes"

# ---------------------------------------------------------------------------
echo
echo "###### 1. build the isolated world (uid $(id -u), no root) ######"
rm -rf "$ROOT" "$SHADOW" "$HARNESS"
mkdir -p "$ROOT" "$SHADOW/files" "$LIBS" "$AI"
cp /data/local/tmp/appiso-run.sh "$AI/run.sh" && chmod +x "$AI/run.sh"
cp /data/local/tmp/appiso-harness.zip "$HARNESS/harness.zip"
"$PREFIX/bin/unzip" -o -j "$APK" 'lib/arm64-v8a/*' -d "$LIBS" >/dev/null 2>&1
printf '%s\n' '[{"name":"uvroot-mt-shadow","note":"only inside the uvroot guest"}]' \
    > "$SHADOW/files/rooms.json"
echo "  mt libs  = $(ls "$LIBS" | tr '\n' ' ')"
echo "  shadow   = $SHADOW (synthetic: the real uid-10107 data is unreadable to us)"

CP="$APK:$HARNESS/harness.zip"

# ---------------------------------------------------------------------------
echo
echo "###### 2. baseline: the same probe with NO uvroot ######"
# Termux's LD_LIBRARY_PATH must go: it shadows /system/lib64 for app_process64
# ("cannot locate symbol Xzs_Construct referenced by libunwindstack.so").
BASELINE=$(env -u LD_LIBRARY_PATH /system/bin/app_process64 \
    -Djava.class.path="$CP" -Djava.library.path="$LIBS" \
    -Dmt.mode=baseline -Dmt.libs="$LIBS" -Dmt.data="$SHADOW" -Dmt.code="$APP_CODE" \
    /system/bin MtProbe 2>&1)
echo "$BASELINE" | sed 's/^/    /'

# ---------------------------------------------------------------------------
echo
echo "###### 3. isolated: the same probe INSIDE the uvroot root ######"
export PKG APP_CODE APP_DATA="$SHADOW" HARNESS UVROOT="$U" GUEST_ROOT="$ROOT"
ISOLATED=$("$PREFIX/bin/sh" "$AI/run.sh" /system/bin/app_process64 \
    -Djava.class.path="/data/app/$PKG/base.apk:/opt/harness/harness.zip" \
    -Djava.library.path=/opt/harness/mtlibs \
    -Dmt.mode=isolated -Dmt.libs=/opt/harness/mtlibs \
    -Dmt.data="/data/data/$PKG" -Dmt.code="/data/app/$PKG" \
    /system/bin MtProbe 2>&1)
echo "$ISOLATED" | sed 's/^/    /'

# ---------------------------------------------------------------------------
echo
echo "###### 4. the effect: baseline vs isolated ######"
for path in /sdcard /storage /storage/emulated/0 /mnt/sdcard /mnt /etc \
            /data/media /data/data/com.termux; do
    ok "MT cannot see $path any more" \
       "$(getv "$BASELINE" "exists:$path"):$(getv "$ISOLATED" "exists:$path")" "true:false"
done
ok "the uvroot mapping invents /data/app/<pkg>/base.apk" \
   "$(getv "$BASELINE" 'exists:/data/app/bin.mt.plus/base.apk'):$(getv "$ISOLATED" 'exists:/data/app/bin.mt.plus/base.apk')" "false:true"
for path in /system/bin/app_process64 /vendor /linkerconfig; do
    ok "still visible in both: $path" \
       "$(getv "$BASELINE" "exists:$path"):$(getv "$ISOLATED" "exists:$path")" "true:true"
done

echo
echo "-- MT Manager's *own* readlink(), baseline vs isolated --"
for pair in "/sdcard:/storage/self/primary" "/mnt/sdcard:/storage/self/primary" "/etc:/system/etc"; do
    path=${pair%%:*}; want=${pair#*:}
    base=$(getv "$BASELINE" "mt.readlink:$path")
    iso=$(getv "$ISOLATED" "mt.readlink:$path")
    echo "     $path : $base  ->  $iso"
    ok "MT readlink($path) resolves outside" "$base" "$want"
    ok "MT readlink($path) is gone inside"  "$iso"  "null"
done

echo
echo "###### 5. MT Manager's own native code agrees ######"
ok "baseline root is the real root (has sdcard)" \
   "$(echo "$BASELINE" | grep -c 'root.entries=.*sdcard')" "1"
ok "isolated root is only the mappings" "$(getv "$ISOLATED" root.entries)" \
   "[apex, data, dev, linkerconfig, opt, proc, sys, system, vendor]"
ok "MT Features.getABI, baseline"  "$(getv "$BASELINE" 'Features.getABI')" "arm64-v8a"
ok "MT Features.getABI, isolated"  "$(getv "$ISOLATED" 'Features.getABI')" "arm64-v8a"
ok "MT uid2name(10107), isolated"  "$(getv "$ISOLATED" 'Features.uid2name(10107)')" "u0_a107"
ok "MT readlink resolves our link, isolated" \
   "$(getv "$ISOLATED" "mt.readlink:/data/data/$PKG/files/mtlink")" "/system"
ok "MT dex Features loaded, isolated"  "$(getv "$ISOLATED" 'dex.Features')"  "ok"
ok "MT dex Features3 loaded, isolated" "$(getv "$ISOLATED" 'dex.Features3')" "ok"
for so in libmt1.so libmt3.so libmtprotect.so libterm.so; do
    ok "MT native $so loads, isolated" "$(getv "$ISOLATED" "dlopen:$so")" "ok"
done

echo
echo "###### 6. standard isolation checks still hold ######"
R() { "$PREFIX/bin/sh" "$AI/run.sh" "$@"; }
RX() { local e=$1; shift; EXTRA="$e" "$PREFIX/bin/sh" "$AI/run.sh" "$@"; }
deny "invisible inside: /sdcard"   R /system/bin/sh -c 'test -e /sdcard'
deny "invisible inside: /storage"  R /system/bin/sh -c 'test -e /storage'
ok   "guest uid == caller uid"     "$(R /system/bin/id -u)" "$(id -u)"
ok   "the shadow is what MT sees"  "$(R /system/bin/cat "/data/data/$PKG/files/rooms.json" | grep -c uvroot-mt-shadow)" "1"
deny "the real MT data is unreadable" ls "/data/data/$PKG"
deny "write refused with --read-only" RX --read-only /system/bin/sh -c "echo x > /data/data/$PKG/files/never"
ok   "nothing was created"         "$(test -e "$SHADOW/files/never" && echo yes || echo no)" "no"
ok   "fake root -i 0:0"            "$(RX '-i 0:0' /system/bin/id -u)" "0"
ok   "host ownership untouched"    "$(stat -c %u "$SHADOW/files/rooms.json")" "$(id -u)"

echo
echo "mt isolation: passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
