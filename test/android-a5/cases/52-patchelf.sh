export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export TMPDIR=$PREFIX/tmp
export UVROOT_TMP_DIR=$PREFIX/tmp
cd $HOME/a5 || exit 1
GLIB=$PREFIX/glibc/bin
which patchelf || ls $GLIB/patchelf 2>/dev/null || find $PREFIX/glibc -maxdepth 2 -name 'patchelf*' 2>/dev/null | head -3
cp -f bins/probe-glibc-termux bins/pg-termux-patched
env -u LD_LIBRARY_PATH PATH="$GLIB:$PREFIX/bin" patchelf \
    --set-interpreter /lib/ld-linux-aarch64.so.1 bins/pg-termux-patched 2>&1 | head -3
echo "rc=$?"
readelf -l bins/pg-termux-patched 2>/dev/null | awk '/interpreter/{print "  interp="$NF}'

P=$HOME/a5/uvroot-ndk; TP=$PREFIX/bin/proot; UBU=$HOME/a5/ubuntu
BINDS="-b /dev -b /proc -b /sys -b $HOME/a5/bins:/tmp/bins"
GP=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
run() { printf "  %-26s %-8s " "$2" "$5"; o=$($1 -r $3 $4 -w /tmp $BINDS /tmp/bins/$2 2>&1); \
  echo "$o" | grep -q "syscall-probe: PASS" && echo PASS || echo "FAIL: $(echo "$o"|head -1|cut -c1-55)"; }
echo
echo "=== glibc 2.44-built dynamic binary (interp repointed) in Ubuntu 24.04 ==="
run $P pg-termux-patched $UBU "-i 0:0" "FORK"
run $TP pg-termux-patched $UBU "-0"     "CONTROL"
echo "=== zig glibc 2.39 dynamic binary (interp already /lib/ld-linux) ==="
run $P probe-glibc-dyn $UBU "-i 0:0" "FORK"
run $TP probe-glibc-dyn $UBU "-0"     "CONTROL"
echo "=== zig musl static ==="
run $P probe-musl-static $HOME/a5/alpine "-i 0:0" "FORK"
run $TP probe-musl-static $HOME/a5/alpine "-0"     "CONTROL"
echo
echo "=== ldd inside guest for the repointed binary ==="
$P -r $UBU -i 0:0 -w /tmp $BINDS /bin/bash -c "export PATH=$GP; ldd /tmp/bins/pg-termux-patched" 2>&1 | head -5
