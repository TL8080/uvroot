#!/bin/bash
# shellcheck disable=SC2012,SC2001  # `ls` on our own dirs; sed indents captured output
# Run an Android application inside a uvroot isolated root, as an ordinary
# unprivileged Termux user.
#
# There is no root anywhere in this suite:
#   * the APK is read straight out of /data/app (world-readable),
#   * the app's "data directory" is a shadow this script creates itself,
#   * the native libraries are unzipped from the APK,
#   * uvroot is started by Termux, as uid 10117.
#
# What the guest sees is only what run.sh binds: /system /apex /vendor
# /linkerconfig, the app's code dir, the shadow data dir, an optional harness
# dir, and /dev /proc /sys.  Everything else -- /sdcard, /etc, /data/media,
# other applications' data -- does not exist inside.
#
#   host:  ./app-isolation/build-and-push.sh
#          ./run-as-termux.sh cases/65-app-isolation.sh
#
set -u
export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets:/system/bin
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export UVROOT_TMP_DIR=$PREFIX/tmp
cd "$HOME/a5" || exit 1

PKG=${PKG:-com.matecal.ceiling}
U=$HOME/a5/uvroot-ndk
BASE=$HOME/a5/appiso
ROOT=$BASE/root
SHADOW=$BASE/shadow
HARNESS=$BASE/harness
AI=$HOME/a5/app-isolation
[ -x "$U" ] || { echo "!! no uvroot at $U"; exit 1; }

pass=0; fail=0
ok()   { if [ "$2" = "$3" ]; then echo "  ok   $1"; pass=$((pass+1));
         else echo "  FAIL $1 (got [$2] want [$3])"; fail=$((fail+1)); fi; }
okp()  { case "$2" in ""|0|0.0) echo "  FAIL $1 ($2)"; fail=$((fail+1));;
                     *) echo "  ok   $1 ($2)"; pass=$((pass+1));; esac; }
deny() { if "$@" >/dev/null 2>&1; then echo "  FAIL $1 (should have been refused)"; fail=$((fail+1));
         else echo "  ok   $1"; pass=$((pass+1)); fi; }

# ---------------------------------------------------------------------------
echo "###### 0. discover the application (no root) ######"
APK=$(/system/bin/pm path "$PKG" 2>/dev/null | sed -n 's/^package://p' | head -1)
[ -n "$APK" ] || { echo "!! $PKG is not installed"; exit 1; }
APP_CODE=$(dirname "$APK")
echo "  pkg       = $PKG"
echo "  code dir  = $APP_CODE"

# ---------------------------------------------------------------------------
echo
echo "###### 1. build the isolated world (all as uid $(id -u)) ######"
rm -rf "$ROOT" "$SHADOW" "$HARNESS"
mkdir -p "$ROOT" "$SHADOW/files" "$SHADOW/shared_prefs" "$HARNESS/libs" "$AI"
cp /data/local/tmp/appiso-run.sh "$AI/run.sh" && chmod +x "$AI/run.sh"
cp /data/local/tmp/appiso-harness.zip "$HARNESS/harness.zip"
"$PREFIX/bin/unzip" -o -j "$APK" 'lib/arm64-v8a/*' -d "$HARNESS/libs" >/dev/null 2>&1
# A synthetic data directory: nothing here comes from the real app.
printf '%s\n' '[{"name":"uvroot-shadow-room","dim":"4800x3600mm","crossDir":0,"panelDir":0}' \
              '"note":"this file only exists inside the uvroot guest"}]' > "$SHADOW/files/rooms.json"
printf '%s\n' '<?xml version="1.0" encoding="utf-8" standalone="yes" ?>' '<map>' \
              '  <string name="shadow">yes</string>' '</map>' > "$SHADOW/shared_prefs/matecal.xml"
echo "  shadow rooms.json : $(cat "$SHADOW/files/rooms.json")"
echo "  native libs       : $(ls "$HARNESS/libs" | tr '\n' ' ')"

export PKG APP_CODE APP_DATA=$SHADOW HARNESS UVROOT=$U GUEST_ROOT=$ROOT
R()  { "$PREFIX/bin/sh" "$AI/run.sh" "$@"; }
RX() { local e=$1; shift; EXTRA="$e" "$PREFIX/bin/sh" "$AI/run.sh" "$@"; }

# ---------------------------------------------------------------------------
echo
echo "###### 2. the virtual root ######"
ok "root entries are only the mappings" \
   "$(R /system/bin/sh -c 'ls -1 /' | tr '\n' ' ' | sed 's/ $//')" \
   "apex data dev linkerconfig opt proc sys system vendor"
for p in /sdcard /storage /etc /home /tmp /root /data/media /data/data/com.termux; do
    deny "invisible inside: $p" R /system/bin/sh -c "test -e $p"
done
ok "visible inside: the app's data dir"  "$(R /system/bin/sh -c "test -d /data/data/$PKG && echo yes")" "yes"
ok "visible inside: the app's APK"       "$(R /system/bin/sh -c "test -f /data/app/$PKG/base.apk && echo yes")" "yes"
ok "/data/data holds only the target"    "$(R /system/bin/sh -c 'ls -1 /data/data')" "$PKG"
deny "the host /data/app is unlistable"  ls /data/app
deny "the real data dir is unreadable"   ls "/data/data/$PKG"
ok "guest uid == caller uid (no fake root)" "$(R /system/bin/id -u)" "$(id -u)"

# ---------------------------------------------------------------------------
echo
echo "###### 3. the application's own code, inside the guest ######"
CP="/data/app/$PKG/base.apk:/opt/harness/harness.zip"
AP=$(R /system/bin/app_process64 \
        -Djava.class.path="$CP" \
        -Djava.library.path=/opt/harness/libs \
        -Dapp.pkg="$PKG" -Dapp.data="/data/data/$PKG" -Dapp.libs=/opt/harness/libs \
        /system/bin AppProbe 2>&1)
echo "$AP" | sed 's/^/    /'
get() { echo "$AP" | sed -n "s/^PROBE: $1=//p" | head -1; }
cnt() { echo "$AP" | grep -c "$1"; }

ok  "AppProbe: zero failures"        "$(get summary.fail)" "0"
ok  "app engine calcPanelGrid.full"  "$(get calcPanelGrid.full)" "48"
okp "app engine calcRoom.areaM2"     "$(get calcRoom.areaM2)"
okp "app engine calcRoom.mainKeelQty" "$(get calcRoom.mainKeelQty)"
okp "app engine calcAll.totalArea"   "$(get calcAll.totalArea)"
ok  "app engine calcAll.panelsOpt"   "$(get calcAll.panelsOpt)" "48"
okp "app geometry LayoutVerts.build" "$(get LayoutVerts.build.floats)"
ok  "native lib libceilingvk.so"     "$(cnt 'PROBE-OK   dlopen:libceilingvk.so')" "1"
ok  "native lib libc++_shared.so"    "$(cnt 'PROBE-OK   dlopen:libc++_shared.so')" "1"
ok  "reads the shadow rooms.json"    "$(cnt uvroot-shadow-room)" "1"

# ---------------------------------------------------------------------------
echo
echo "###### 4. writes stay in the shadow ######"
ok "the probe's write landed in the shadow" \
   "$(test -s "$SHADOW/files/uvroot-probe-marker.txt" && echo yes || echo no)" "yes"
ok "shadow still holds only our file set" \
   "$(ls -1 "$SHADOW/files" | tr '\n' ' ' | sed 's/ $//')" "rooms.json uvroot-probe-marker.txt"

# ---------------------------------------------------------------------------
echo
echo "###### 5. read-only mapping ######"
deny "write refused with --read-only" \
     RX --read-only /system/bin/sh -c "echo x > /data/data/$PKG/files/never"
ok "nothing was created" "$(test -e "$SHADOW/files/never" && echo yes || echo no)" "no"
ok "--read-only still reads" \
   "$(RX --read-only /system/bin/cat "/data/data/$PKG/files/rooms.json" | grep -c uvroot-shadow-room)" "1"

# ---------------------------------------------------------------------------
echo
echo "###### 6. fake identity ######"
ok "guest uid 0 with -i 0:0"        "$(RX '-i 0:0' /system/bin/id -u)" "0"
ok "host file ownership untouched"  "$(stat -c %u "$SHADOW/files/rooms.json")" "$(id -u)"
ok "uid 0 guest still reads shadow" \
   "$(RX '-i 0:0' /system/bin/cat "/data/data/$PKG/files/rooms.json" | grep -c uvroot-shadow-room)" "1"

# ---------------------------------------------------------------------------
echo
echo "###### 7. the GUI boundary ######"
echo "-- an app_process started by uvroot is not an application process:"
WP=$(R /system/bin/app_process64 -Djava.class.path=/opt/harness/harness.zip \
        /system/bin WindowProbe 2>&1)
echo "$WP" | sed 's/^/    /'
ok "isolated ART has no ActivityThread" \
   "$(echo "$WP" | grep -c 'ActivityThread.currentActivityThread=null')" "1"
ok "isolated ART has no Application" \
   "$(echo "$WP" | grep -c 'ActivityThread.currentApplication=null')" "1"

echo "-- /system/bin/am from inside the guest:"
AM=$(R /system/bin/am start -n "$PKG/.MainActivity" 2>&1)
echo "    $AM"

echo "-- binder startActivityAsUser from inside the guest:"
AS=$(R /system/bin/app_process64 -Djava.class.path=/opt/harness/harness.zip \
        -Dapp.pkg="$PKG" /system/bin AmStart 2>&1)
echo "$AS" | sed 's/^/    /'
echo "    (AMS's verdict on this request only shows up in the host logcat)"

# ---------------------------------------------------------------------------
echo
echo "app isolation: passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
