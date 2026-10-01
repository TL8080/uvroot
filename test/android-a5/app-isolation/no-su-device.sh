#!/system/bin/sh
# shellcheck disable=SC2012  # `ls` is fine here: we list directories we created
# Run on the board as the adb *shell* user (uid 2000): no su, no Termux, no
# root anywhere.  The point is that uvroot needs no privilege at all -- the
# only thing the uid decides is which files it happens to be allowed to read.
#
#   host: ./no-su-check.sh
#
# The APK is world-readable, the "data directory" is a shadow we create under
# /data/local/tmp, and the native libraries come out of the APK with unzip.
B=/data/local/tmp/appiso-nosu
PKG=com.matecal.ceiling
U=/data/local/tmp/uvroot

pass=0; fail=0
ok()  { if [ "$2" = "$3" ]; then echo "  ok   $1"; pass=$((pass+1));
        else echo "  FAIL $1 (got [$2] want [$3])"; fail=$((fail+1)); fi; }
okp() { case "$2" in ""|0|0.0) echo "  FAIL $1 ($2)"; fail=$((fail+1));;
                    *) echo "  ok   $1 ($2)"; pass=$((pass+1));; esac; }
deny(){ if "$@" >/dev/null 2>&1; then echo "  FAIL $1 (should have been refused)"; fail=$((fail+1));
        else echo "  ok   $1"; pass=$((pass+1)); fi; }

echo "###### 0. who are we ######"
ok "running as the adb shell user, no su" "$(id -u)" "2000"
[ -x "$U" ] || { echo "!! no uvroot at $U"; exit 1; }

echo
echo "###### 1. setup, entirely unprivileged ######"
rm -rf "$B"
mkdir -p "$B/root" "$B/shadow/files" "$B/harness/libs" || exit 1
APK=$(pm path "$PKG" 2>/dev/null | sed -n 's/^package://p' | head -1)
[ -n "$APK" ] || { echo "!! $PKG is not installed"; exit 1; }
CODE=$(dirname "$APK")
cp /data/local/tmp/appiso-harness.zip "$B/harness/harness.zip"
unzip -o -j "$APK" 'lib/arm64-v8a/*' -d "$B/harness/libs" >/dev/null 2>&1
printf '%s\n' '[{"name":"uvroot-nosu-shadow","note":"written by uid 2000"}]' \
    > "$B/shadow/files/rooms.json"
echo "  apk      = $APK"
echo "  shadow   = $B/shadow"
echo "  libs     = $(ls "$B/harness/libs" | tr '\n' ' ')"

export PKG APP_CODE="$CODE" APP_DATA="$B/shadow" HARNESS="$B/harness"
export UVROOT="$U" GUEST_ROOT="$B/root"
export UVROOT_TMP_DIR=/data/local/tmp TMPDIR=/data/local/tmp
R() { sh /data/local/tmp/appiso-run.sh "$@"; }

echo
echo "###### 2. the virtual root ######"
ok "root entries are only the mappings" \
   "$(R /system/bin/sh -c 'ls -1 /' | tr '\n' ' ' | sed 's/ $//')" \
   "apex data dev linkerconfig opt proc sys system vendor"
for p in /sdcard /storage /etc /home /tmp /root /data/media /data/data/com.termux; do
    deny "invisible inside: $p" R /system/bin/sh -c "test -e $p"
done
ok "the guest uid is still 2000" "$(R /system/bin/id -u)" "2000"
deny "the real data dir is unreadable to uid 2000" ls "/data/data/$PKG"

echo
echo "###### 3. the app's code, inside the guest ######"
CP="/data/app/$PKG/base.apk:/opt/harness/harness.zip"
AP=$(R /system/bin/app_process64 -Djava.class.path="$CP" -Djava.library.path=/opt/harness/libs \
        -Dapp.pkg="$PKG" -Dapp.data="/data/data/$PKG" -Dapp.libs=/opt/harness/libs \
        /system/bin AppProbe 2>&1)
get() { echo "$AP" | sed -n "s/^PROBE: $1=//p" | head -1; }
cnt() { echo "$AP" | grep -c "$1"; }
ok  "AppProbe: zero failures"       "$(get summary.fail)" "0"
ok  "app engine calcPanelGrid.full" "$(get calcPanelGrid.full)" "48"
okp "app engine calcAll.totalArea"  "$(get calcAll.totalArea)"
okp "app geometry LayoutVerts.build" "$(get LayoutVerts.build.floats)"
ok  "native lib libceilingvk.so"    "$(cnt 'PROBE-OK   dlopen:libceilingvk.so')" "1"
ok  "reads the shadow rooms.json"   "$(cnt uvroot-nosu-shadow)" "1"

echo
echo "###### 4. writes stay in the shadow ######"
ok "the probe's write landed in the shadow" \
   "$(test -s "$B/shadow/files/uvroot-probe-marker.txt" && echo yes || echo no)" "yes"
ok "files in the shadow" "$(ls -1 "$B/shadow/files" | tr '\n' ' ' | sed 's/ $//')" \
   "rooms.json uvroot-probe-marker.txt"

echo
echo "no-su app isolation: passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
