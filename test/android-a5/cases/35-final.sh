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
echo "--- 1. exact shell sequence (no tar) ---"
rm -rf /tmp/zz; mkdir -p /tmp/zz; cd /tmp/zz
mkdir ./etc; echo "  mkdir ./etc rc=$? exists=$([ -d ./etc ] && echo yes || echo no)"
mkdir ./etc/ssl; echo "  mkdir ./etc/ssl rc=$?"
ls -la /tmp/zz
echo "--- 2. tar plain ---"
rm -rf /tmp/z1; mkdir -p /tmp/z1; cd /tmp/z1; tar -xf /tmp/o.tar 2>/tmp/e1; echo "  rc=$? entries=$(find /tmp/z1 | wc -l)"
echo "--- 3. tar --no-same-owner ---"
rm -rf /tmp/z2; mkdir -p /tmp/z2; cd /tmp/z2; tar -xf /tmp/o.tar --no-same-owner 2>/tmp/e2; echo "  rc=$? entries=$(find /tmp/z2 | wc -l)"
echo "--- 4. tar --numeric-owner ---"
rm -rf /tmp/z3; mkdir -p /tmp/z3; cd /tmp/z3; tar -xf /tmp/o.tar --numeric-owner 2>/tmp/e3; echo "  rc=$? entries=$(find /tmp/z3 | wc -l)"
echo "--- 5. tar -v (slow but verbose) first errors ---"
rm -rf /tmp/z4; mkdir -p /tmp/z4; cd /tmp/z4; tar -xvf /tmp/o.tar 2>&1 | head -6'
echo "########## OUR uvroot ##########"; $P -r $UBU -i 0:0 -w /tmp $BINDS /bin/bash -c "$S" 2>&1
echo "########## CONTROL ##########"; $TP -r $UBU -0 -w /tmp $BINDS /bin/bash -c "$S" 2>&1
