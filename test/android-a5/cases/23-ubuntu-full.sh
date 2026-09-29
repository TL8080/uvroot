export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export UVROOT_TMP_DIR=$PREFIX/tmp
cd $HOME/a5 || exit 1
P=$HOME/a5/uvroot-ndk; UBU=$HOME/a5/ubuntu
GP=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
BINDS="-b /dev -b /proc -b /sys -b $HOME/a5/probe.c:/tmp/probe.c"
APT="export PATH=$GP; sed -i 's#http://mirrors.ustc.edu.cn#https://mirrors.ustc.edu.cn#g' /etc/apt/sources.list.d/ubuntu.sources 2>/dev/null; dpkg --configure -a >/dev/null 2>&1; apt-get update 2>&1 | tail -4"

echo "=== 1. apt-get update over USTC https (fixed uvroot) ==="
timeout 600 $P -r $UBU -i 0:0 -w / $BINDS /bin/bash -c "$APT"

echo "=== 2. install ca-certificates + gcc + libc6-dev ==="
time timeout 1800 $P -r $UBU -i 0:0 -w / $BINDS /bin/bash -c "export PATH=$GP; apt-get install -y --no-install-recommends ca-certificates gcc libc6-dev 2>&1 | tail -5"
echo "  rc=$?"

echo "=== 3. compile + run probe inside glibc/Ubuntu ==="
$P -r $UBU -i 0:0 -w /tmp $BINDS /bin/bash -c "export PATH=$GP; gcc --version | head -1; gcc -O2 -Wall /tmp/probe.c -o /tmp/probe && /tmp/probe; echo rc=\$?; echo '--- ldd ---'; ldd /tmp/probe" 2>&1 | head -14
