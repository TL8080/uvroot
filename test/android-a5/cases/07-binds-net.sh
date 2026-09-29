export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export UVROOT_TMP_DIR=$PREFIX/tmp
cd $HOME/a5 || exit 1

P=$HOME/a5/uvroot-ndk
TP=$PREFIX/bin/proot
ALP=$HOME/a5/alpine
UBU=$HOME/a5/ubuntu
GP=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
BINDS="-b /dev -b /proc -b /sys"

echo "############ E. /proc,/dev,/sys binding ############"
echo "--- E1 alpine with binds ---"
$P -r $ALP -i 0:0 -w / $BINDS /bin/sh -c "export PATH=$GP; head -2 /proc/self/status; echo dev: \$(ls /dev | head -4 | tr '\n' ' ')" 2>&1 | head -6
echo "--- E2 ubuntu with binds ---"
$P -r $UBU -i 0:0 -w / $BINDS /bin/bash -c "export PATH=$GP; head -2 /proc/self/status; echo dev: \$(ls /dev | head -4 | tr '\n' ' ')" 2>&1 | head -6
echo "--- E3 alpine with -R alias ---"
$P -R $ALP -i 0:0 -w / /bin/sh -c "export PATH=$GP; head -1 /proc/self/status; head -1 /etc/os-release" 2>&1 | head -6
echo "  rc=$?"

echo
echo "############ F. ca-certificates presence ############"
echo "--- F1 alpine ---"; ls -l $ALP/etc/ssl/certs/ca-certificates.crt 2>&1; ls $ALP/etc/ssl 2>&1 | head -3
echo "--- F2 ubuntu ---"; ls -l $UBU/etc/ssl/certs/ca-certificates.crt 2>&1 | head -2

echo
echo "############ G. NETWORK inside containers (USTC mirrors) ############"
echo "--- G1 alpine: apk update (https://mirrors.ustc.edu.cn/alpine) ---"
timeout 240 $P -r $ALP -i 0:0 -w / $BINDS /bin/sh -c "export PATH=$GP; apk update" 2>&1 | tail -8
echo "  rc=$?"
echo "--- G2 ubuntu: apt-get update (https://mirrors.ustc.edu.cn/ubuntu-ports) ---"
timeout 420 $P -r $UBU -i 0:0 -w / $BINDS /bin/bash -c "export PATH=$GP; apt-get update" 2>&1 | tail -8
echo "  rc=$?"

echo
echo "############ H. CONTROL termux proot, same binds ############"
echo "--- H1 alpine apk ---"
timeout 240 $TP -r $ALP -0 -w / $BINDS /bin/sh -c "export PATH=$GP; apk update" 2>&1 | tail -4
echo "--- H2 ubuntu apt ---"
timeout 420 $TP -r $UBU -0 -w / $BINDS /bin/bash -c "export PATH=$GP; apt-get update" 2>&1 | tail -4
