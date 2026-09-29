export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export UVROOT_TMP_DIR=$PREFIX/tmp
cd $HOME/a5 || exit 1
P=$HOME/a5/uvroot-ndk; TP=$PREFIX/bin/proot; UBU=$HOME/a5/ubuntu
GP=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
BINDS="-b /dev -b /proc -b /sys"
S='export PATH='"$GP"'
rm -rf /tmp/src /tmp/m.tar; mkdir -p /tmp/src/a/b/c; echo x > /tmp/src/a/f; cd /tmp/src; tar cf /tmp/m.tar ./a
rm -rf /tmp/dst; mkdir -p /tmp/dst; cd /tmp/dst
tar xf /tmp/m.tar 2>/tmp/me; echo "  rc=$? entries=$(find /tmp/dst | wc -l) err=[$(head -1 /tmp/me)]"'
echo "FORK, no id option:      $($P -r $UBU -w /tmp $BINDS /bin/bash -c "$S" 2>&1)"
echo "FORK, -i 0:0:            $($P -r $UBU -i 0:0 -w /tmp $BINDS /bin/bash -c "$S" 2>&1)"
echo "FORK, -S:                $($P -S $UBU -w /tmp -b /dev -b /sys /bin/bash -c "$S" 2>&1)"
echo "FORK, --vperm-id=0:0:    $($P -r $UBU --vperm-id=0:0 -w /tmp $BINDS /bin/bash -c "$S" 2>&1)"
echo "CONTROL, no id option:   $($TP -r $UBU -w /tmp $BINDS /bin/bash -c "$S" 2>&1)"
echo "CONTROL, -0:             $($TP -r $UBU -0 -w /tmp $BINDS /bin/bash -c "$S" 2>&1)"
echo "--- upstream-NDK, -0:     $($HOME/a5/proot-upstream -r $UBU -0 -w /tmp $BINDS /bin/bash -c "$S" 2>&1)"
