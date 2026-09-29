export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
cd $HOME/a5 || exit 1

for c in alpine ubuntu; do
  echo "=== extract $c ==="
  rm -rf $HOME/a5/$c
  mkdir -p $HOME/a5/$c
  tar -xzf $HOME/a5/rootfs/$c.tar.gz -C $HOME/a5/$c
  echo "rc=$? top-level entries: $(ls -A $HOME/a5/$c | wc -l)"
done

echo "=== alpine identity ==="
cat $HOME/a5/alpine/etc/alpine-release
ls -l $HOME/a5/alpine/lib/ld-musl-aarch64.so.1
echo "=== alpine repositories ==="
cat $HOME/a5/alpine/etc/apk/repositories

echo "=== ubuntu identity ==="
grep -E "^(PRETTY_NAME|VERSION_ID)" $HOME/a5/ubuntu/etc/os-release
ls -l $HOME/a5/ubuntu/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1
echo "=== ubuntu resolv.conf ==="
ls -la $HOME/a5/ubuntu/etc/resolv.conf
echo "=== ubuntu sources ==="
cat $HOME/a5/ubuntu/etc/apt/sources.list.d/ubuntu.sources
echo "--- sources.list ---"
cat $HOME/a5/ubuntu/etc/apt/sources.list
