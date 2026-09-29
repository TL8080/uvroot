export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export UVROOT_TMP_DIR=$PREFIX/tmp
cd $HOME/a5 || exit 1
mkdir -p $HOME/a5/bin-ours
ln -sf $HOME/a5/uvroot-ndk $HOME/a5/bin-ours/proot
echo "=== which proot proot-distro will use ==="
PATH=$HOME/a5/bin-ours:$PATH command -v proot
echo "=== install alpine via proot-distro (uses our uvroot (symlinked as 'proot') for login) ==="
time timeout 1800 env PATH=$HOME/a5/bin-ours:$PATH proot-distro install alpine 2>&1 | tail -12
echo "=== login test ==="
timeout 300 env PATH=$HOME/a5/bin-ours:$PATH proot-distro login alpine -- /bin/sh -c 'export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; head -1 /etc/os-release; uname -m; echo hello-from-alpine; ls -la / | head -4' 2>&1 | head -12
