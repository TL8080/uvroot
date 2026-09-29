#!/bin/bash
# Read/write isolation (--read-only, --ro=<path>) on the Android board.
#
# Device-adapted version of the --ro/--read-only part of
# test/test-image-guard.sh.  The upstream suite uses ext4 images + debugfs;
# on the board there is no loop/ext4 tooling, so the locked trees are plain
# directories reached through -b bindings and verified from the host side.
#
#   run from the host with:
#     ./run-as-termux.sh cases/62-readonly-device.sh
#
set -u
export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets
export LD_LIBRARY_PATH=$PREFIX/lib
export TMPDIR=$PREFIX/tmp
export UVROOT_TMP_DIR=$PREFIX/tmp
cd "$HOME/a5" || exit 1

U=$HOME/a5/uvroot-ndk
SRC=$HOME/a5/alpine
VT=$HOME/a5/ro
R=$VT/root
RW=$VT/rwdir
RO=$VT/rodir
TREE=$VT/tree
GP=/bin:/usr/bin:/sbin:/usr/sbin

pass=0; fail=0
ok()  { if [ "$2" = "$3" ]; then echo "  ok   $1"; pass=$((pass+1));
        else echo "  FAIL $1 (got [$2] want [$3])"; fail=$((fail+1)); fi; }
deny() { # $1 name, rest: command that must fail
  if "$@" >/dev/null 2>&1; then echo "  FAIL $1 (should have been refused)"; fail=$((fail+1));
  else echo "  ok   $1"; pass=$((pass+1)); fi; }

rm -rf "$VT"; mkdir -p "$R" "$RW" "$RO" "$TREE/sub/deeper"
cp -a "$SRC"/. "$R"/ 2>/dev/null
echo v > "$TREE/sub/deeper/keep"
mkdir -p "$R/t"

# fake root so that "even id0 is refused" is meaningful
u()  { PATH=$GP $U -r "$R" -i 0:0 "$@" 2>/dev/null; }
ue() { { PATH=$GP $U -r "$R" -i 0:0 "$@" >/dev/null; } 2>&1; }

echo "############ read/write isolation on Android/arm64 (Alpine guest) ############"
echo

echo "== --ro on one binding only =="
deny "--ro refuses a write on the locked bind" \
     u --ro=/ro -b "$RW:/rw" -b "$RO:/ro" /bin/sh -c 'echo x > /ro/f'
u --ro=/ro -b "$RW:/rw" -b "$RO:/ro" /bin/sh -c 'echo x > /rw/f' >/dev/null
ok "the other bind stays writable"  "$(test -f "$RW/f" && echo yes)" "yes"
ok "nothing reached the locked dir" "$(test -e "$RO/f" && echo yes || echo no)" "no"
ok "reads on the locked dir work"   "$(u --ro=/ro -b "$RO:/ro" /bin/ls /ro >/dev/null 2>&1 && echo yes)" "yes"

echo "== recursion and child processes =="
deny "--ro covers sub-directories (rm)"    u --ro=/tree -b "$TREE:/tree" /bin/sh -c 'rm -f /tree/sub/deeper/keep'
deny "--ro covers sub-directories (mkdir)" u --ro=/tree -b "$TREE:/tree" /bin/mkdir /tree/sub/deeper/d
deny "--ro covers chmod"                   u --ro=/tree -b "$TREE:/tree" /bin/chmod 777 /tree/sub/deeper/keep
ok "the file survived"        "$(test -f "$TREE/sub/deeper/keep" && echo yes)" "yes"
ok "its mode is unchanged"    "$(stat -c %a "$TREE/sub/deeper/keep")" "644"
ok "stat works under --ro"    "$(u --ro=/tree -b "$TREE:/tree" /bin/stat -c %s /tree/sub/deeper/keep)" "2"
ok "exec works under --ro"    "$(u --ro=/tree -b "$TREE:/tree" /bin/ls /tree >/dev/null 2>&1 && echo yes)" "yes"

echo "== --ro=/ locks the whole rootfs =="
deny "--ro=/ refuses writes on the rootfs" u --ro=/ /bin/sh -c 'echo x > /t/nope'
deny "--ro=/ refuses mkdir on the rootfs"  u --ro=/ /bin/mkdir /t/nodir
ok "--ro=/ still allows reads"  "$(u --ro=/ /bin/cat /etc/alpine-release | tr -d '\n')" "3.24.2"
ok "--ro=/ still allows exec"   "$(u --ro=/ /bin/ls / >/dev/null 2>&1 && echo yes)" "yes"
ok "nothing was created"        "$(test -e "$R/t/nope" -o -e "$R/t/nodir" && echo yes || echo no)" "no"

echo "== --read-only (whole container) =="
deny "--read-only refuses a write"   u --read-only /bin/sh -c 'echo x > /t/ro1'
deny "--read-only refuses mkdir"     u --read-only /bin/mkdir /t/ro2
deny "--read-only refuses unlink"    u --read-only /bin/rm -f /etc/alpine-release
deny "--read-only refuses truncate"  u --read-only /bin/sh -c 'echo big > /t/ro3'
ok "--read-only still reads"  "$(u --read-only /bin/cat /etc/alpine-release | tr -d '\n')" "3.24.2"
ok "--read-only still execs"  "$(u --read-only /bin/ls / >/dev/null 2>&1 && echo yes)" "yes"
ok "no file left behind"      "$(test -e "$R/t/ro1" -o -e "$R/t/ro2" -o -e "$R/t/ro3" && echo yes || echo no)" "no"
ok "the rootfs file survived" "$(test -f "$R/etc/alpine-release" && echo yes)" "yes"

echo "== even id0 is refused =="
deny "id0 + --ro"          u --vperm-id=0:0 --ro=/ro -b "$RW:/rw" -b "$RO:/ro" /bin/sh -c 'echo x > /ro/f'
deny "id0 + --read-only"   u --vperm-id=0:0 --read-only /bin/sh -c 'echo x > /t/ro4'
ok "id0 can still write elsewhere" \
   "$(u --vperm-id=0:0 --ro=/ro -b "$RW:/rw" -b "$RO:/ro" /bin/sh -c 'echo ok > /rw/id0 && cat /rw/id0')" "ok"

echo "== a missing --ro path is ignored, with a warning =="
warn=$(ue --ro=/tree/nope -b "$TREE:/tree" /bin/mkdir /tree/nope)
ok "warning mentions it is ignored" "$(printf '%s' "$warn" | grep -c 'is ignored')" "1"
ok "the mkdir then succeeded"       "$(test -d "$TREE/nope" && echo yes)" "yes"

echo "== a rule survives removal from outside =="
mkdir -p "$TREE/recreate"
( PATH=$GP $U -r "$R" -i 0:0 --ro=/tree -b "$TREE:/tree" \
      /bin/sh -c '/bin/sleep 3; /bin/mkdir /tree/recreate; echo rc=$?' \
      > "$VT/recreate.out" 2>&1 ) &
pid=$!
/bin/sleep 1
rmdir "$TREE/recreate"
wait $pid 2>/dev/null
ok "recreation from inside is refused" "$(grep -c 'rc=[1-9]' "$VT/recreate.out")" "1"
ok "and it was not recreated"          "$(test -d "$TREE/recreate" && echo yes || echo no)" "no"

echo "== --ro does not leak into a later run =="
u /bin/sh -c 'echo after > /t/ro-after' >/dev/null
ok "writable again without the flag" "$(test -f "$R/t/ro-after" && echo yes)" "yes"

echo
echo "read-only: passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
