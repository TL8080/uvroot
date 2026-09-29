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
cd /tmp/dl 2>/dev/null || { mkdir -p /tmp/dl && cd /tmp/dl; }
sha256sum openssl*.deb
for i in 1 2 3; do
  rm -rf /tmp/x$i; mkdir -p /tmp/x$i
  dpkg-deb -x openssl*.deb /tmp/x$i > /tmp/e$i.txt 2>&1
  echo "run $i rc=$? bytes=$(du -s /tmp/x$i | cut -f1) errs=$(wc -l < /tmp/e$i.txt)"
  head -2 /tmp/e$i.txt
done'
echo "########## OUR uvroot (FIXED) ##########"
$P -r $UBU -i 0:0 -w /tmp $BINDS /bin/bash -c "$S" 2>&1 | head -25
echo "########## CONTROL ##########"
$TP -r $UBU -0 -w /tmp $BINDS /bin/bash -c "$S" 2>&1 | head -20
