export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export UVROOT_TMP_DIR=$PREFIX/tmp
cd $HOME/a5 || exit 1
P=$HOME/a5/uvroot-ndk; ALP=$HOME/a5/alpine
GP=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
BINDS="-b /dev -b /proc -b /sys -b $HOME/a5/probe.c:/tmp/probe.c"

echo "=== J1 install build-base (musl) ==="
time timeout 1500 $P -r $ALP -i 0:0 -w / $BINDS /bin/sh -c "export PATH=$GP; apk add --no-cache build-base 2>&1 | tail -8"
echo "  rc=$?"

echo "=== K1 compile + run (musl) ==="
$P -r $ALP -i 0:0 -w /tmp $BINDS /bin/sh -c "export PATH=$GP; gcc --version | head -1; gcc -O2 -Wall /tmp/probe.c -o /tmp/probe && /tmp/probe; echo rc=\$?; echo '--- ldd ---'; ldd /tmp/probe" 2>&1 | head -20
