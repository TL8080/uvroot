export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export UVROOT_TMP_DIR=$PREFIX/tmp
cd $HOME/a5 || exit 1
P=$HOME/a5/uvroot-ndk; ALP=$HOME/a5/alpine
GP=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
echo "=== exact arg form proot-distro v4 builds (proot_cmd.py) ==="
$P --change-id=0:0 --link2symlink --kill-on-exit -r $ALP -w / -b /dev -b /proc -b /sys \
   /bin/sh -c "export PATH=$GP; head -1 /etc/os-release; id -u; echo ARGFORM-OK" 2>&1 | head -6
echo "=== legacy -0 (upstream flag, removed in this fork) ==="
$P -r $ALP -0 -w / /bin/true 2>&1 | head -2
