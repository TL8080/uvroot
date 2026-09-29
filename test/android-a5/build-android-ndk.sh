#!/usr/bin/env bash
#
# Cross-build this uvroot fork for Android/arm64 with the Android NDK, using
# the Termux libtalloc-static package (aarch64/bionic) as the talloc provider.
#
# Usage:
#   NDK=/path/to/ndk build-android-ndk.sh [api] [outdir]
#
# Produces $OUT/uvroot (aarch64 Android PIE executable).
#
set -euo pipefail

NDK="${NDK:-$HOME/Android/Sdk/ndk/27.2.12479018}"
API="${1:-24}"
OUT="${2:-$(pwd)/out-android}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
MIRROR="${USTC_TERMUX:-https://mirrors.ustc.edu.cn/termux/apt/termux-main}"
TALLOC_VER=2.4.3

log() { printf '\033[1m==> %s\033[0m\n' "$*"; }

[ -x "$TC/aarch64-linux-android${API}-clang" ] || {
    echo "NDK toolchain not found under $TC (API $API)" >&2
    exit 1
}

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------- talloc ----
log "fetching Termux libtalloc ${TALLOC_VER} from USTC"
SYSROOT="$WORK/sysroot"
mkdir -p "$SYSROOT/usr/lib/pkgconfig" "$SYSROOT/usr/include" "$SYSROOT/usr/lib"

fetch_deb() { # $1 = package dir, $2 = deb filename
    curl -fsSL --max-time 120 -o "$WORK/$2" \
        "$MIRROR/pool/main/libt/$1/$2"
}
fetch_deb libtalloc        "libtalloc_${TALLOC_VER}_aarch64.deb"
fetch_deb libtalloc-static "libtalloc-static_${TALLOC_VER}_aarch64.deb"

rm -rf "$WORK/x"; mkdir -p "$WORK/x"
( cd "$WORK/x" && ar x "$WORK/libtalloc_${TALLOC_VER}_aarch64.deb" && tar xJf data.tar.xz )
cp "$WORK/x/data/data/com.termux/files/usr/include/talloc.h" "$SYSROOT/usr/include/"
sed 's#^prefix=.*#prefix=/usr#' \
    "$WORK/x/data/data/com.termux/files/usr/lib/pkgconfig/talloc.pc" \
    > "$SYSROOT/usr/lib/pkgconfig/talloc.pc"
rm -rf "$WORK/y"; mkdir -p "$WORK/y"
( cd "$WORK/y" && ar x "$WORK/libtalloc-static_${TALLOC_VER}_aarch64.deb" && tar xJf data.tar.xz )
cp "$WORK/y/data/data/com.termux/files/usr/lib/libtalloc.a" "$SYSROOT/usr/lib/"

# ----------------------------------------------------------- toolchain ------
log "wrapping the NDK toolchain for CROSS_COMPILE=<prefix>"
WRAP="$WORK/toolchain"; mkdir -p "$WRAP"
P="aarch64-linux-android${API}-"
mk() { printf '#!/bin/sh\nexec %s "$@"\n' "$2" > "$WRAP/$1"; chmod +x "$WRAP/$1"; }
mk "${P}gcc"     "$TC/aarch64-linux-android${API}-clang"
mk "${P}ld"      "$TC/ld.lld"
mk "${P}strip"   "$TC/llvm-strip"
mk "${P}objcopy" "$TC/llvm-objcopy"
mk "${P}objdump" "$TC/llvm-objdump"
cat > "$WRAP/${P}pkg-config" <<EOF
#!/bin/sh
# A host PKG_CONFIG_PATH (e.g. a netfs-deps sysroot) would shadow our
# PKG_CONFIG_LIBDIR and silently enable host-only backends such as libcurl.
unset PKG_CONFIG_PATH
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
exec pkg-config "\$@"
EOF
chmod +x "$WRAP/${P}pkg-config"

# Keep the parent environment from leaking into pkg-config / make.
unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR PKG_CONFIG_LIBDIR 2>/dev/null || true

# --------------------------------------------------------------- build ------
# NOTE: build in a clean *copy* of the tree.  Building in-tree with
# `make -f /repo/src/GNUmakefile` from another cwd makes GNU make find the
# already-built x86_64 ./uvroot through VPATH and report "up to date".
log "copying sources to a clean tree and building"
SRC="$WORK/src"
mkdir -p "$SRC"
tar -C "$REPO" -cf - --exclude=.git --exclude=m2 --exclude='*.o' \
    --exclude=src/uvroot --exclude=src/care src lib doc contrib \
    AUTHORS COPYING CHANGELOG.rst README.rst 2>/dev/null | tar -C "$SRC" -xf -

mkdir -p "$OUT"
make -C "$SRC/src" uvroot \
     CROSS_COMPILE="$WRAP/$P" \
     WITHOUT_PYTHON=1

"$TC/llvm-strip" "$SRC/src/uvroot"
cp "$SRC/src/uvroot" "$OUT/uvroot"
log "built $OUT/uvroot"
file "$OUT/uvroot"
"$TC/llvm-readelf" -d "$OUT/uvroot" | grep NEEDED || true
