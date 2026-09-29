export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export DEBIAN_FRONTEND=noninteractive
cd $HOME || exit 1
echo "=== install zig ==="
time apt install -y zig 2>&1 | tail -6
echo "=== version ==="
zig version
echo "=== supported targets (grep musl/gnu aarch64) ==="
zig targets 2>/dev/null | head -40
