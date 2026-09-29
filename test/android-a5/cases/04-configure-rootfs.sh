export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
cd $HOME/a5 || exit 1

DNS='nameserver 223.5.5.5
nameserver 119.29.29.29'

# ---------- Alpine (musl) ----------
ALP=$HOME/a5/alpine
ALPV=$(cut -d. -f1,2 $ALP/etc/alpine-release)
cat > $ALP/etc/apk/repositories <<EOS
https://mirrors.ustc.edu.cn/alpine/v$ALPV/main
https://mirrors.ustc.edu.cn/alpine/v$ALPV/community
EOS
rm -f $ALP/etc/resolv.conf
printf '%s\n' "$DNS" > $ALP/etc/resolv.conf
echo "=== alpine repositories ==="; cat $ALP/etc/apk/repositories
echo "=== alpine resolv.conf ==="; cat $ALP/etc/resolv.conf

# ---------- Ubuntu (glibc) ----------
UBU=$HOME/a5/ubuntu
rm -f $UBU/etc/apt/sources.list.d/ubuntu.sources
cat > $UBU/etc/apt/sources.list.d/ubuntu.sources <<'EOS'
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
EOS
rm -f $UBU/etc/resolv.conf
printf '%s\n' "$DNS" > $UBU/etc/resolv.conf
echo "=== ubuntu sources ==="; cat $UBU/etc/apt/sources.list.d/ubuntu.sources
echo "=== ubuntu resolv.conf ==="; cat $UBU/etc/resolv.conf

# ---------- install our uvroot into the Termux home ----------
cp -f /data/local/tmp/uvroot $HOME/a5/uvroot-ndk 2>/dev/null && chmod 755 $HOME/a5/uvroot-ndk
cp -f /data/local/tmp/a5run $HOME/a5/a5run 2>/dev/null && chmod 755 $HOME/a5/a5run
echo "=== a5 dir ==="; ls -la $HOME/a5/
echo "=== uvroot-ndk ==="; $HOME/a5/uvroot-ndk --version | tail -1
