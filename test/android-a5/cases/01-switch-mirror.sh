export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export TERM=xterm-256color
cd $HOME || exit 1

echo "=== BEFORE sources.list ==="
cat $PREFIX/etc/apt/sources.list

if [ ! -f $PREFIX/etc/apt/sources.list.orig ]; then
  cp -a $PREFIX/etc/apt/sources.list $PREFIX/etc/apt/sources.list.orig
  echo "(backup -> sources.list.orig)"
fi

cat > $PREFIX/etc/apt/sources.list <<'EOS'
# Termux main repository (USTC mirror, Hefei)
deb https://mirrors.ustc.edu.cn/termux/apt/termux-main/ stable main
EOS

echo "=== AFTER sources.list ==="
cat $PREFIX/etc/apt/sources.list
echo "=== apt update ==="
apt update 2>&1 | tail -20
echo "UPDATE_RC=$?"
