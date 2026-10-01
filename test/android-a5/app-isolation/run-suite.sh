#!/usr/bin/env bash
# Host side: one-command reproduction.
#
#   DEVICE=<adb serial> ./run-suite.sh [timeout]
#
# 1. builds the probes and pushes them,
# 2. runs cases/65-app-isolation.sh on the board as the Termux user (no root),
# 3. collects the system-side evidence for the GUI boundary, which the suite
#    itself cannot see from inside the guest.
#
# The only privileged steps are read-only observations made from the host
# (`su -c ls` of another process's root); the experiment proper uses no root.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
A5=$(cd "$HERE/.." && pwd)
DEVICE=${DEVICE:?set DEVICE to the adb serial}
PKG=${PKG:-com.matecal.ceiling}
ADBADDR="adb -s $DEVICE"
TIMEOUT=${1:-900}

say() { printf '\n================ %s ================\n' "$*"; }

say "build and push the probe harness"
ADB="$ADBADDR" "$HERE/build-and-push.sh" || exit 1

say "run cases/65-app-isolation.sh on the board (Termux uid, no root)"
DEVICE="$DEVICE" "$A5/run-as-termux.sh" "$A5/cases/65-app-isolation.sh" "$TIMEOUT"
suite_rc=$?

# ---------------------------------------------------------------------------
say "GUI boundary, part 1: what an AMS-launched app process really is"
$ADBADDR logcat -c 2>/dev/null
$ADBADDR shell am force-stop "$PKG"
$ADBADDR shell am start -n "$PKG/.MainActivity" >/dev/null 2>&1
sleep 5
PID=$($ADBADDR shell pidof "$PKG" | tr -d '\r')
echo "app pid = ${PID:-<not running>}"
$ADBADDR shell dumpsys activity activities 2>/dev/null | grep -m1 topResumedActivity
if [ -n "${PID:-}" ]; then
    echo "-- the uvroot mapping (/opt/harness) inside that process (expect 0):"
    $ADBADDR shell "grep -c /opt/harness /proc/$PID/mountinfo" 2>/dev/null
    echo "-- paths its root has and the uvroot guest does not:"
    $ADBADDR shell "su -c 'for p in sdcard storage etc data/media; do printf \"%s=%s \" \$p \$([ -e /proc/$PID/root/\$p ] && echo yes || echo no); done; echo'" 2>/dev/null
    echo "-- /data/data in its own namespace (Android filters that per app already):"
    $ADBADDR shell "su -c 'ls /proc/$PID/root/data/data'" 2>/dev/null
    echo "-- the rooms.json it actually reads:"
    $ADBADDR shell "su -c 'head -c 70 /proc/$PID/root/data/data/$PKG/files/rooms.json'" 2>/dev/null; echo
    echo "   (the guest in section 3 of the suite saw 'uvroot-shadow-room' instead)"
fi

# ---------------------------------------------------------------------------
say "GUI boundary, part 2: a launch attempt issued from inside the guest"
$ADBADDR logcat -c 2>/dev/null
cat > /tmp/appiso-amstart.sh <<'EOS'
export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets:/system/bin
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp UVROOT_TMP_DIR=$PREFIX/tmp
PKG=com.matecal.ceiling
APK=$(/system/bin/pm path "$PKG" | sed -n 's/^package://p' | head -1)
export PKG APP_CODE=$(dirname "$APK") APP_DATA=$HOME/a5/appiso/shadow HARNESS=$HOME/a5/appiso/harness
export UVROOT=$HOME/a5/uvroot-ndk GUEST_ROOT=$HOME/a5/appiso/root
"$PREFIX/bin/sh" "$HOME/a5/app-isolation/run.sh" \
    /system/bin/app_process64 -Djava.class.path=/opt/harness/harness.zip \
    -Dapp.pkg=$PKG /system/bin AmStart
EOS
DEVICE="$DEVICE" "$A5/run-as-termux.sh" /tmp/appiso-amstart.sh 120
echo
echo "-- what ActivityTaskManagerService logged about it:"
sleep 2
$ADBADDR logcat -d 2>/dev/null | grep -E 'ActivityTaskManager|Background activity start' \
    | grep -iE 'matecal|from uid' | tail -8

say "result"
echo "cases/65-app-isolation.sh exit code = $suite_rc"
exit "$suite_rc"
