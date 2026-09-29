export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export UVROOT_TMP_DIR=$PREFIX/tmp
cd $HOME/a5 || exit 1
mkdir -p bins

echo "=== compile probe.c with Termux gcc-glibc 14.2.1 (glibc 2.44) ==="
$PREFIX/glibc/bin/aarch64-linux-gnu-gcc -O2 probe.c -o bins/probe-glibc-termux 2>&1 | head -5
echo "rc=$?"
readelf -l bins/probe-glibc-termux 2>/dev/null | awk '/interpreter/{print "  interp="$NF}'
readelf -d bins/probe-glibc-termux 2>/dev/null | awk '/NEEDED/{print "  needed="$NF}'
echo "  max glibc symbol version required:"
readelf -V bins/probe-glibc-termux 2>/dev/null | grep -oE "GLIBC_[0-9.]+" | sort -V -u | tail -3 | sed 's/^/    /'

P=$HOME/a5/uvroot-ndk; TP=$PREFIX/bin/proot; U2=$HOME/a5/ubuntu
BINDS="-b /dev -b /proc -b /sys -b $HOME/a5/bins:/tmp/bins"
GP=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

echo
echo "=== run under FORK (fixed) in Ubuntu 24.04 (glibc 2.39) ==="
$P -r $U2 -i 0:0 -w /tmp $BINDS /bin/bash -c "export PATH=$GP; /tmp/bins/probe-glibc-termux; echo rc=\$?" 2>&1 | head -6
echo "=== run under CONTROL in Ubuntu 24.04 ==="
$TP -r $U2 -0 -w /tmp $BINDS /bin/bash -c "export PATH=$GP; /tmp/bins/probe-glibc-termux; echo rc=\$?" 2>&1 | head -6
echo
echo "=== run under FORK with the TERMUX glibc as rootfs (uses \$PREFIX/glibc) ==="
$P -r $PREFIX/glibc -i 0:0 -w /tmp $BINDS /bin/true 2>&1 | head -3
