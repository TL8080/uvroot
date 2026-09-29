export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export UVROOT_TMP_DIR=$PREFIX/tmp
cd $HOME/a5 || exit 1
P=$HOME/a5/uvroot-ndk; TP=$PREFIX/bin/proot; U2=$HOME/a5/ubuntu2
GP=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
DNS='nameserver 223.5.5.5
nameserver 119.29.29.29'

echo "=== fresh extract ubuntu2 ==="
rm -rf $U2; mkdir -p $U2
tar -xzf $HOME/a5/rootfs/ubuntu.tar.gz -C $U2 && echo "extracted"
cat > $U2/etc/apt/sources.list.d/ubuntu.sources <<'EOF'
Types: deb
URIs: https://mirrors.ustc.edu.cn/ubuntu-ports/
Suites: noble noble-updates noble-backports
Components: main universe restricted multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

Types: deb
URIs: https://mirrors.ustc.edu.cn/ubuntu-ports/
Suites: noble-security
Components: main universe restricted multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF
rm -f $U2/etc/resolv.conf; printf '%s\n' "$DNS" > $U2/etc/resolv.conf
mkdir -p $U2/etc/ssl/certs; cp /data/local/tmp/ca-bundle.pem $U2/etc/ssl/certs/ca-certificates.crt 2>/dev/null

BINDS="-b /dev -b /proc -b /sys -b $HOME/a5/probe.c:/tmp/probe.c"

echo "=== install gcc with CONTROL uvroot (rootfs pristine) ==="
time timeout 2400 $TP -r $U2 -0 -w / $BINDS /bin/bash -c "export PATH=$GP; apt-get update 2>&1 | tail -2; apt-get install -y --no-install-recommends ca-certificates gcc libc6-dev 2>&1 | tail -3"

echo "=== compile + run probe under FORK (fixed) ==="
timeout 600 $P -r $U2 -i 0:0 -w /tmp $BINDS /bin/bash -c "export PATH=$GP; gcc --version | head -1; gcc -O2 -Wall /tmp/probe.c -o /tmp/probe && /tmp/probe; echo rc=\$?; ldd /tmp/probe" 2>&1 | head -12

echo "=== compile + run probe under CONTROL (reference) ==="
timeout 600 $TP -r $U2 -0 -w /tmp $BINDS /bin/bash -c "export PATH=$GP; gcc -O2 /tmp/probe.c -o /tmp/p2 && /tmp/p2; echo rc=\$?" 2>&1 | tail -5

echo "=== apt-get update under FORK on the SAME fresh rootfs (has gcc now) ==="
timeout 600 $P -r $U2 -i 0:0 -w / $BINDS /bin/bash -c "export PATH=$GP; apt-get update 2>&1 | tail -3"
