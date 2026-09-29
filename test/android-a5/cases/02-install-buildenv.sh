export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export TERM=xterm-256color
export DEBIAN_FRONTEND=noninteractive
cd $HOME || exit 1

echo "=== apt update ==="
apt update 2>&1 | tail -5

echo "=== install build environment ==="
apt install -y \
  clang make binutils pkg-config \
  libtalloc libtalloc-static \
  git curl wget tar xz-utils unzip zip \
  proot-distro 2>&1 | tail -40
RC=$?
echo "INSTALL_RC=$RC"

echo "=== versions ==="
clang --version 2>&1 | head -2
make --version 2>&1 | head -1
pkg-config --modversion talloc 2>&1
bash --version | head -1
proot-distro --version 2>&1 | head -1
echo "=== disk after ==="
df -h $PREFIX | tail -1
