export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
cd $HOME/a5 || exit 1
export UVROOT_TMP_DIR=$PREFIX/tmp

P=$HOME/a5/uvroot-ndk
ALP=$HOME/a5/alpine
UBU=$HOME/a5/ubuntu
GPATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

run_alp() { $P -r "$ALP" -i 0:0 -w / /bin/sh -c "export PATH=$GPATH; $1" 2>&1; echo "  rc=$?"; }
run_ubu() { $P -r "$UBU" -i 0:0 -w / /bin/bash -c "export PATH=$GPATH; $1" 2>&1; echo "  rc=$?"; }

echo "############ A. CLI COMPATIBILITY ############"
echo "--- A1: standard -0 flag (upstream proot-distro uses this) ---"
$P -r $ALP -0 -w / /bin/true 2>&1 | head -2; echo "  rc=$?"
echo "--- A2: -i 0:0 (fork equivalent) ---"
$P -r $ALP -i 0:0 -w / /bin/true 2>&1 | head -2; echo "  rc=$?"

echo
echo "############ B. musl / Alpine 3.24 ############"
echo "--- B1 identity ---"; run_alp 'uname -m; id; head -1 /etc/os-release'
echo "--- B2 dynamic musl binary ---"; run_alp 'busybox | head -1; ls -l /bin/ | head -3'
echo "--- B3 file I/O ---"; run_alp 'echo hello-musl > /tmp/t.txt && cat /tmp/t.txt && stat -c %s /tmp/t.txt'
echo "--- B4 fork/exec/pipe ---"; run_alp 'for i in 3 1 2; do echo $i; done | sort | tr "\n" ","'
echo "--- B5 /proc ---"; run_alp 'head -2 /proc/self/status'
echo "--- B6 signals ---"; run_alp 'sh -c "trap \"echo caught\" TERM; kill -TERM \$\$; sleep 1"'
echo "--- B7 write to /etc as fake root ---"; run_alp 'echo marker > /etc/a5test && cat /etc/a5test'

echo
echo "############ C. glibc / Ubuntu 24.04 ############"
echo "--- C1 identity ---"; run_ubu 'uname -m; id; grep PRETTY_NAME /etc/os-release'
echo "--- C2 dynamic glibc binary ---"; run_ubu 'ls -l / | head -3; /bin/ls --version | head -1'
echo "--- C3 file I/O ---"; run_ubu 'echo hello-glibc > /tmp/t.txt && cat /tmp/t.txt && stat -c %s /tmp/t.txt'
echo "--- C4 fork/exec/pipe ---"; run_ubu 'for i in 3 1 2; do echo $i; done | sort | tr "\n" ","'
echo "--- C5 /proc ---"; run_ubu 'head -2 /proc/self/status'
echo "--- C6 signals ---"; run_ubu 'bash -c "trap \"echo caught\" TERM; kill -TERM \$\$; sleep 1"'
echo "--- C7 write to /etc as fake root ---"; run_ubu 'echo marker > /etc/a5test && cat /etc/a5test'

echo
echo "############ D. CONTROL: termux packaged proot ############"
TP=$PREFIX/bin/proot
echo "--- D1 alpine ---"; $TP -r $ALP -0 -w / /bin/sh -c "export PATH=$GPATH; head -1 /etc/os-release; echo io > /tmp/z && cat /tmp/z" 2>&1 | head -5; echo "  rc=$?"
echo "--- D2 ubuntu ---"; $TP -r $UBU -0 -w / /bin/bash -c "export PATH=$GPATH; grep PRETTY_NAME /etc/os-release; echo io > /tmp/z && cat /tmp/z" 2>&1 | head -5; echo "  rc=$?"
echo "--- D3 termux proot version ---"; $TP --version 2>&1 | tail -1
