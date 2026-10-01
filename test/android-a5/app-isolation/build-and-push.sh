#!/usr/bin/env bash
# Host side: compile the two probes, dex them into one harness.zip and push it
# to the board.  Everything else (the run itself) happens on the device and
# needs no root.
#
#   ANDROID_SDK=~/Android/Sdk  JAVA_HOME=~/tools/jdk-21.0.2  ./build-and-push.sh
#
# The zip lands in /data/local/tmp/; cases/65-app-isolation.sh copies it into
# the Termux home and adds the app's native libraries.
set -euo pipefail

SDK=${ANDROID_SDK:-$HOME/Android/Sdk}
JAVA_HOME=${JAVA_HOME:-$HOME/tools/jdk-21.0.2}
ADB=${ADB:-adb}

D8=$SDK/build-tools/34.0.0/d8
AJAR=$SDK/platforms/android-34/android.jar
JAVAC=$JAVA_HOME/bin/javac
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$(mktemp -d)}

[ -x "$JAVAC" ] || { echo "no javac at $JAVAC (set JAVA_HOME)"; exit 1; }
[ -x "$D8" ]    || { echo "no d8 at $D8 (set ANDROID_SDK)"; exit 1; }

# javac 21, Java-8 bytecode: the smallest common denominator ART accepts.
"$JAVAC" -source 8 -target 8 -nowarn -d "$OUT" "$HERE/AppProbe.java" "$HERE/MtProbe.java" 2>/dev/null
for c in WindowProbe AmStart; do
    "$JAVAC" -source 8 -target 8 -nowarn -cp "$AJAR" -d "$OUT" "$HERE/$c.java" 2>/dev/null
done

# java.nio.file.Files needs API 26; the board is API 33.
"$D8" --min-api 26 --output "$OUT/harness.zip" \
      "$OUT/AppProbe.class" "$OUT/MtProbe.class" "$OUT/WindowProbe.class" "$OUT/AmStart.class"

echo "built $OUT/harness.zip:"
unzip -l "$OUT/harness.zip"

$ADB push "$OUT/harness.zip" /data/local/tmp/appiso-harness.zip
$ADB shell chmod 644 /data/local/tmp/appiso-harness.zip
$ADB push "$HERE/run.sh" /data/local/tmp/appiso-run.sh
$ADB shell chmod 644 /data/local/tmp/appiso-run.sh
echo "pushed -> /data/local/tmp/appiso-harness.zip and /data/local/tmp/appiso-run.sh"
