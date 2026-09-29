export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export TMPDIR=$PREFIX/tmp
export UVROOT_TMP_DIR=$PREFIX/tmp
cd $HOME/a5 || exit 1
mkdir -p bins
GLIB=$PREFIX/glibc/bin

echo "=== build with Termux gcc-glibc (no LD_LIBRARY_PATH!) ==="
env -u LD_LIBRARY_PATH PATH="$GLIB:$PREFIX/bin" \
    $GLIB/aarch64-linux-gnu-gcc -O2 probe.c -o bins/probe-glibc-termux
echo "rc=$? size=$(wc -c < bins/probe-glibc-termux 2>/dev/null)"
readelf -l bins/probe-glibc-termux 2>/dev/null | awk '/interpreter/{print "  interp="$NF}'
readelf -d bins/probe-glibc-termux 2>/dev/null | awk '/NEEDED/{print "  needed="$NF}'
echo -n "  glibc symbol versions used: "
readelf -V bins/probe-glibc-termux 2>/dev/null | grep -oE "GLIBC_[0-9.]+" | sort -V -u | tr '\n' ' '; echo
echo "  static twin:"
env -u LD_LIBRARY_PATH PATH="$GLIB:$PREFIX/bin" \
    $GLIB/aarch64-linux-gnu-gcc -O2 -static probe.c -o bins/probe-glibc-termux-static 2>&1 | head -2
ls -la bins/ | tail -6

P=$HOME/a5/uvroot-ndk; TP=$PREFIX/bin/proot; UBU=$HOME/a5/ubuntu
BINDS="-b /dev -b /proc -b /sys -b $HOME/a5/bins:/tmp/bins"
GP=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
run() { printf "  %-38s " "$2"; o=$($1 -r $3 $4 -w /tmp $BINDS /tmp/bins/$2 2>&1); \
  echo "$o" | grep -q "syscall-probe: PASS" && echo PASS || echo "FAIL: $(echo "$o"|head -1|cut -c1-60)"; }

echo
echo "=== run under FORK (fixed) in Ubuntu 24.04 ==="
run $P probe-glibc-termux       $UBU "-i 0:0"
run $P probe-glibc-termux-static $UBU "-i 0:0"
echo "=== run under CONTROL in Ubuntu 24.04 ==="
run $TP probe-glibc-termux       $UBU "-0"
run $TP probe-glibc-termux-static $UBU "-0"
