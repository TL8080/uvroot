export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export DEBIAN_FRONTEND=noninteractive
cd $HOME || exit 1
echo "=== add termux-glibc repo (only official servers carry it) ==="
apt install -y glibc-repo 2>&1 | tail -3
cat $PREFIX/etc/apt/sources.list.d/glibc.list | head -2
apt update 2>&1 | tail -4
echo "=== install glibc toolchain ==="
time apt install -y glibc gcc-glibc binutils-glibc 2>&1 | tail -6
echo "=== layout ==="
ls -d $PREFIX/glibc 2>/dev/null && ls $PREFIX/glibc/bin 2>/dev/null | head -20
echo "=== gcc-glibc version ==="
export PATH=$PREFIX/glibc/bin:$PATH
gcc --version 2>&1 | head -1
ldd --version 2>&1 | head -2
