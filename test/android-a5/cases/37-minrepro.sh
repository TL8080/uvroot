export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export UVROOT_TMP_DIR=$PREFIX/tmp
cd $HOME/a5 || exit 1
P=$HOME/a5/uvroot-ndk; U=$HOME/a5/proot-upstream; TP=$PREFIX/bin/proot; UBU=$HOME/a5/ubuntu
GP=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
BINDS="-b /dev -b /proc -b /sys"
S='export PATH='"$GP"'
echo "--- build tiny archive with ./a/b/c ---"
rm -rf /tmp/src /tmp/m.tar; mkdir -p /tmp/src/a/b/c; echo x > /tmp/src/a/f; cd /tmp/src
tar cf /tmp/m.tar ./a
tar tf /tmp/m.tar
echo "--- extract cwd-relative ---"
rm -rf /tmp/dst; mkdir -p /tmp/dst; cd /tmp/dst
tar xf /tmp/m.tar 2>/tmp/me; echo "rc=$?"; echo "stderr:[$(cat /tmp/me)]"
echo "on-disk:"; find /tmp/dst | sort
echo "--- extract with -C ---"
rm -rf /tmp/dst2; mkdir -p /tmp/dst2; tar xf /tmp/m.tar -C /tmp/dst2 2>/tmp/me2; echo "rc=$? stderr:[$(cat /tmp/me2)]"; find /tmp/dst2 | sort'
echo "########## FORK (fixed) ##########"; $P -r $UBU -i 0:0 -w /tmp $BINDS /bin/bash -c "$S" 2>&1
echo "########## CONTROL ##########";     $TP -r $UBU -0 -w /tmp $BINDS /bin/bash -c "$S" 2>&1
