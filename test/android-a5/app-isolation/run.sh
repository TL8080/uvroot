#!/data/data/com.termux/files/usr/bin/sh
# Launch a command inside a uvroot root that contains ONLY the mappings below.
#
# This is the whole trick: the guest root is an *empty* directory, and the only
# things that exist inside it are what we bind.  No root, no kernel module, no
# system-wide setup -- uvroot is an ordinary unprivileged process.
#
#   PKG        package name                            (default com.matecal.ceiling)
#   APP_CODE   versioned install directory of the app  (required)
#   APP_DATA   directory used as /data/data/$PKG       (required; usually a shadow)
#   HARNESS    optional directory exposed at /opt/harness (dex + native libs)
#   UVROOT     uvroot binary                           (default $HOME/a5/uvroot-ndk)
#   GUEST_ROOT empty dir used as the guest /          (default $HOME/a5/appiso/root)
#   EXTRA      extra uvroot flags, e.g. "-i 0:0 --read-only"
#
# Inside the guest these are the only top-level entries:
#   apex data dev linkerconfig opt proc sys system vendor
set -u

PKG=${PKG:-com.matecal.ceiling}
# NB: keep this message free of apostrophes -- dash parses the word of a
# ${VAR:?word} expansion, and a lone quote there runs to end of file.
APP_CODE=${APP_CODE:?APP_CODE must point at the versioned install dir}
APP_DATA=${APP_DATA:?APP_DATA must point at the directory to use as data}
HARNESS=${HARNESS:-}
UVROOT=${UVROOT:-$HOME/a5/uvroot-ndk}
GUEST_ROOT=${GUEST_ROOT:-$HOME/a5/appiso/root}
EXTRA=${EXTRA:-}

# uvroot writes its embedded loader here; Android has no /tmp.
export UVROOT_TMP_DIR="${UVROOT_TMP_DIR:-${TMPDIR:-/data/local/tmp}}"
export TMPDIR="$UVROOT_TMP_DIR"

FLAGS="-r $GUEST_ROOT -b /system -b /apex -b /vendor -b /linkerconfig"
FLAGS="$FLAGS -b $APP_CODE:/data/app/$PKG -b $APP_DATA:/data/data/$PKG"
if [ -n "$HARNESS" ]; then
    FLAGS="$FLAGS -b $HARNESS:/opt/harness"
fi
FLAGS="$FLAGS -b /dev -b /proc -b /sys -w /"

# shellcheck disable=SC2086  # word splitting is intended: build the argv
exec "$UVROOT" $FLAGS ${EXTRA:-} "$@"
