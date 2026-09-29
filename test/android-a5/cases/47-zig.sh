export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export UVROOT_TMP_DIR=$PREFIX/tmp
export ZIG_GLOBAL_CACHE_DIR=$HOME/.cache/zig
cd $HOME/a5 || exit 1
mkdir -p bins

echo "=== build with zig (musl + glibc targets) ==="
set -x
zig cc -target aarch64-linux-musl           -O2 probe.c -o bins/probe-musl-dyn
zig cc -target aarch64-linux-musl   -static -O2 probe.c -o bins/probe-musl-static
zig cc -target aarch64-linux-gnu.2.39       -O2 probe.c -o bins/probe-glibc-dyn
zig cc -target aarch64-linux-gnu.2.39 -static -O2 probe.c -o bins/probe-glibc-static
set +x
echo "=== file / interpreter ==="
for f in bins/probe-*; do
  printf "%-28s %s\n" "$(basename $f)" "$(file -b $f | cut -c1-60)"
  intp=$(readelf -l "$f" 2>/dev/null | awk '/interpreter/{print $NF}' | tr -d ':')
  [ -n "$intp" ] && printf "%-28s   interp=%s\n" "" "$intp"
  printf "%-28s   needed=%s\n" "" "$(readelf -d $f 2>/dev/null | awk '/NEEDED/{print $NF}' | tr -d '[]' | tr '\n' ' ')"
done

P=$HOME/a5/uvroot-ndk; TP=$PREFIX/bin/proot; ALP=$HOME/a5/alpine; UBU=$HOME/a5/ubuntu
BINDS="-b /dev -b /proc -b /sys -b $HOME/a5/bins:/tmp/bins"
GP=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

run() { # $1 uvroot-binary, $2 flag, $3 rootfs, $4 bin
  printf "  %-44s " "$(basename $4) in $(basename $3)"
  out=$($1 -r $3 $2 -w /tmp $BINDS /tmp/bins/$(basename $4) 2>&1)
  rc=$?
  if echo "$out" | grep -q "syscall-probe: PASS"; then echo "PASS"
  else echo "FAIL(rc=$rc): $(echo "$out" | head -1 | cut -c1-70)"; fi
}

echo
echo "=== FORK (fixed, NDK) ==="
for b in probe-musl-dyn probe-musl-static probe-glibc-dyn probe-glibc-static; do
  if [ "$b" = "probe-musl-dyn" ] || [ "$b" = "probe-musl-static" ]; then r=$ALP; else r=$UBU; fi
  run $P "-i 0:0" $r $HOME/a5/bins/$b
done
echo "=== CONTROL (termux pkg) ==="
for b in probe-musl-dyn probe-musl-static probe-glibc-dyn probe-glibc-static; do
  if [ "$b" = "probe-musl-dyn" ] || [ "$b" = "probe-musl-static" ]; then r=$ALP; else r=$UBU; fi
  run $TP "-0" $r $HOME/a5/bins/$b
done
echo
echo "=== cross-negative control (wrong libc for the rootfs) ==="
run $P "-i 0:0" $UBU $HOME/a5/bins/probe-musl-dyn
run $P "-i 0:0" $ALP $HOME/a5/bins/probe-glibc-dyn
