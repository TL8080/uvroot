export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export DEBIAN_FRONTEND=noninteractive
cd $HOME || exit 1
echo "=== install glibc-runner ==="
apt install -y glibc-runner 2>&1 | tail -4
echo "=== what it provides ==="
ls -la $PREFIX/bin | grep -iE "glibc|grun" | head
echo "=== glibc dir layout ==="
ls $PREFIX/glibc/ 2>/dev/null
ls -la $PREFIX/glibc/lib/libc.so $PREFIX/glibc/lib/libc.so.6 2>/dev/null
echo "=== head of libc.so ==="
head -c 120 $PREFIX/glibc/lib/libc.so; echo
echo "=== try gcc without LD_LIBRARY_PATH ==="
env -u LD_LIBRARY_PATH $PREFIX/glibc/bin/aarch64-linux-gnu-gcc --version 2>&1 | head -2
