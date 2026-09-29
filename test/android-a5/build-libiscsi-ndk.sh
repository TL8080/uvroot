#!/usr/bin/env bash
#
# Cross-build libiscsi -- the user-space iSCSI initiator that uvroot's
# --iscsi backend dlopen()s -- for Android/arm64 with the Android NDK.
#
# The GitHub tag archive ships no generated configure script, so instead of
# pulling in autotools this compiles the plain-C sources directly with the
# same configuration the Autotools build would choose on Linux: no GnuTLS
# and no libgcrypt (the bundled MD5 is used for CHAP), no iSER/RDMA
# (lib/iser.c is left out), pthread support on.  The soname and the library
# file name are taken from lib/Makefile.am (-version-info), so they follow
# the upstream version.
#
# Usage:
#   NDK=/path/to/ndk build-libiscsi-ndk.sh [api] [outdir]
#
# Environment:
#   LIBISCSI_VERSION    upstream tag (default 1.20.3, soname 11)
#   LIBISCSI_TARBALL    use this tarball instead of downloading it
#   LIBISCSI_SHA256     expected tarball hash (GitHub archives can be
#                       re-generated; override when the download changes)
#
# Produces in $OUT:
#   lib/libiscsi.so.<major>.<age>.<revision>, lib/libiscsi.so.<major>,
#   lib/libiscsi.so, lib/pkgconfig/libiscsi.pc and the public headers in
#   include/iscsi/ -- pass $OUT to build-android-ndk.sh's EXTRA_DEPS_DIRS
#   so that uvroot's --iscsi backend is compiled in (it dlopen()s the
#   library at run time, so the .so only has to be present on the device).
#
set -euo pipefail

NDK="${NDK:-$HOME/Android/Sdk/ndk/27.2.12479018}"
API="${1:-24}"
OUT="${2:-$(pwd)/out-libiscsi}"
VERSION="${LIBISCSI_VERSION:-1.20.3}"
URL="${LIBISCSI_URL:-https://github.com/sahlberg/libiscsi/archive/refs/tags/${VERSION}.tar.gz}"
SHA256="${LIBISCSI_SHA256:-212f6e1fd8e7ddb4b02208aafc6de600f6f330f40359babeefdd83b0c79d47a1}"
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="$TC/aarch64-linux-android${API}-clang"

log() { printf '\033[1m==> %s\033[0m\n' "$*"; }

[ -x "$CC" ] || { echo "NDK toolchain not found: $CC" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------- source ----
if [ -n "${LIBISCSI_TARBALL:-}" ]; then
    log "using $LIBISCSI_TARBALL"
    cp "$LIBISCSI_TARBALL" "$WORK/libiscsi.tar.gz"
else
    log "downloading libiscsi ${VERSION}"
    curl -fsSL --max-time 180 -o "$WORK/libiscsi.tar.gz" "$URL"
fi

actual="$(sha256sum "$WORK/libiscsi.tar.gz" | cut -d' ' -f1)"
if [ "$actual" != "$SHA256" ]; then
    echo "sha256 mismatch for libiscsi ${VERSION}:" >&2
    echo "  expected $SHA256" >&2
    echo "  actual   $actual" >&2
    echo "Set LIBISCSI_SHA256=$actual (or LIBISCSI_TARBALL=...) if the" >&2
    echo "archive was legitimately re-generated." >&2
    exit 1
fi

SRC="$WORK/libiscsi-$VERSION"
tar xzf "$WORK/libiscsi.tar.gz" -C "$WORK"
[ -d "$SRC" ] || { echo "unexpected archive layout in $SRC" >&2; exit 1; }

# ------------------------------------------------------- version numbers ----
# libtool -version-info current:revision:age
current="$(sed -n 's/^SOCURRENT=//p' "$SRC/lib/Makefile.am")"
revision="$(sed -n 's/^SOREVISON=//p' "$SRC/lib/Makefile.am")"
age="$(sed -n 's/^SOAGE=//p' "$SRC/lib/Makefile.am")"
if [ -z "$current" ] || [ -z "$revision" ] || [ -z "$age" ]; then
    echo "cannot read SOCURRENT/SOREVISON/SOAGE from lib/Makefile.am" >&2
    exit 1
fi
major=$((current - age))
SONAME="libiscsi.so.$major"
REAL="libiscsi.so.$major.$age.$revision"
log "libiscsi ${VERSION}: soname $SONAME, file $REAL"

# ---------------------------------------------------------------- compile ---
# Everything configure would have put in config.h for a Linux/Android
# target; the macros that would disable features (HAVE_LINUX_ISER,
# HAVE_LIBGNUTLS, HAVE_LIBGCRYPT, HAVE_SOCK_SIN_LEN, HAVE_DISPATCH_*,
# HAVE_PTHREAD_THREADID_NP) are deliberately absent.
DEFINES=(
    -DHAVE_ARPA_INET_H -DHAVE_INTTYPES_H -DHAVE_NETINET_IN_H
    -DHAVE_NETINET_TCP_H -DHAVE_POLL_H -DHAVE_STDATOMIC_H
    -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_SYS_SELECT_H
    -DHAVE_SYS_SOCKET_H -DHAVE_SYS_TIME_H -DHAVE_SYS_TYPES_H
    -DHAVE_SYS_UIO_H -DHAVE_UNISTD_H -DHAVE_SOCKADDR_IN6
    -DHAVE_PTHREAD -DHAVE_MULTITHREADING
)

# shellcheck disable=SC2012
SRCS="$(find "$SRC/lib" -maxdepth 1 -name '*.c' ! -name 'iser.c' | sort)"

log "compiling $(echo "$SRCS" | wc -l) files for aarch64 (API $API)"
mkdir -p "$OUT/lib" "$OUT/include"
# shellcheck disable=SC2086
"$CC" -O2 -fPIC -shared -I"$SRC/include" "${DEFINES[@]}" \
    -Wl,-soname,"$SONAME" -o "$OUT/lib/$REAL" $SRCS

ln -sf "$REAL" "$OUT/lib/$SONAME"
ln -sf "$SONAME" "$OUT/lib/libiscsi.so"

# Same install layout as "make install": the two public headers under
# include/iscsi/ plus a pkg-config file, so this directory can be merged
# into another NDK build's sysroot with EXTRA_DEPS_DIRS.
mkdir -p "$OUT/include/iscsi" "$OUT/lib/pkgconfig"
cp "$SRC/include/iscsi.h" "$SRC/include/scsi-lowlevel.h" "$OUT/include/iscsi/"
cat > "$OUT/lib/pkgconfig/libiscsi.pc" <<EOF
prefix=/usr
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libiscsi
Description: iSCSI initiator library
Version: $VERSION
Libs: -L\${libdir} -liscsi
Cflags: -I\${includedir}
EOF

"$TC/llvm-strip" "$OUT/lib/$REAL"

log "built $OUT/lib/$REAL"
file "$OUT/lib/$REAL"
"$TC/llvm-readelf" -d "$OUT/lib/$REAL" | grep -E 'SONAME|NEEDED' || true
printf 'exports iscsi_create_context: '
"$TC/llvm-nm" -D --defined-only "$OUT/lib/$REAL" | grep -c ' iscsi_create_context$' || true
