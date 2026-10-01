#!/usr/bin/env bash
# Host side: run the app-isolation checks as the plain adb shell user.
#
#   DEVICE=<adb serial> ./no-su-check.sh
#
# There is no `su` in this file, and none in no-su-device.sh: the whole
# experiment runs as uid 2000.  That is the strongest form of the "uvroot needs
# no privilege" claim -- the Termux run in cases/65-app-isolation.sh still uses
# `su` once, only to acquire the Termux uid and its supplementary groups.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
DEVICE=${DEVICE:?set DEVICE to the adb serial}
ADB=(adb -s "$DEVICE")

"${ADB[@]}" push "$HERE/no-su-device.sh" /data/local/tmp/appiso-nosu.sh >/dev/null || exit 1
"${ADB[@]}" shell chmod 644 /data/local/tmp/appiso-nosu.sh
"${ADB[@]}" shell sh /data/local/tmp/appiso-nosu.sh
