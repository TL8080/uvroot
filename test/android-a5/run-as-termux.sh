#!/usr/bin/env bash
# Run a local script on the device as the Termux app uid, via the NDK-built
# a5run helper (which supplies AID_INET etc. so Android netd allows DNS).
#
#   DEVICE=<ADB_SERIAL> ./run-as-termux.sh cases/06-matrix.sh [timeout]
#
set -uo pipefail
DEVICE="${DEVICE:-<ADB_SERIAL>}"
ADB=(adb -s "$DEVICE")
SRC="$1"; TO="${2:-900}"

"${ADB[@]}" push "$SRC" /data/local/tmp/job.sh >/dev/null 2>&1 || { echo "PUSH FAILED"; exit 1; }
"${ADB[@]}" shell "chmod 644 /data/local/tmp/job.sh"
timeout "$TO" "${ADB[@]}" shell \
  "su -c '/data/local/tmp/a5run /data/data/com.termux/files/usr/bin/bash /data/local/tmp/job.sh'"
echo "[exit=$?]"
