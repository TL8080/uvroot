#!/bin/bash
# Is the vperm database (.uvroot-vperm) really read-only inside the container?
#
# Threat model: the container is untrusted.  It must not be able to read,
# change, delete or replace the database, because the database is what
# enforces the virtual identities -- a guest that can rewrite it can grant
# itself any ownership it likes.  Only two writers are legitimate:
#
#   * the host, from outside the container, and
#   * uvroot itself (the tracer) while maintaining the database.
#
# Two bypasses were found and fixed while writing this suite:
#
#   1. a hard link to the database -- the check compared path strings, so
#      /t/dblink (same inode) was not recognized and gave read *and* write
#      access to the database;
#   2. <db>.tmp -- db_save() wrote it with fopen(..., "w"), which follows a
#      symlink, so a symlink planted at that name made uvroot write the
#      database over an arbitrary host file.
#
#   run from the host with:
#     ./run-as-termux.sh cases/64-vperm-db-protection.sh
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
VT=$HOME/a5/dbguard
R=$VT/root
GP=/bin:/usr/bin:/sbin:/usr/sbin
DB=/.uvroot-vperm
HOSTDB=$R/.uvroot-vperm

pass=0; fail=0
ok()  { if [ "$2" = "$3" ]; then echo "  ok   $1"; pass=$((pass+1));
        else echo "  FAIL $1 (got [$2] want [$3])"; fail=$((fail+1)); fi; }
deny() { if "$@" >/dev/null 2>&1; then echo "  FAIL $1 (should have been refused)"; fail=$((fail+1));
         else echo "  ok   $1"; pass=$((pass+1)); fi; }

rm -rf "$VT"; mkdir -p "$R"
cp -a "$SRC"/. "$R"/ 2>/dev/null
mkdir -p "$R/t"
printf 'secret\n' > "$R/t/data.txt"
printf 'IMPORTANT-DATA\n' > "$R/t/victim"

g()  { PATH=$GP $U -r "$R" -i 0:0 "$@" 2>/dev/null; }
e()  { { PATH=$GP $U -r "$R" -i 0:0 "$@" >/dev/null; } 2>&1; }
# virtual root attacks (the strongest guest identity)
a0() { PATH=$GP $U -r "$R" -i 0:0 --vperm --vperm-id=0 "$@" 2>/dev/null; }
d0() { deny "$1" env PATH=$GP $U -r "$R" -i 0:0 --vperm --vperm-id=0 "${@:2}"; }

echo "############ vperm database protection on Android/arm64 ############"
g --vperm /bin/sh -c 'chmod 700 /t/data.txt' >/dev/null
DB_BEFORE=$(md5sum "$HOSTDB" | cut -c1-32)
echo "  db: $HOSTDB"
echo "  size=$(stat -c %s "$HOSTDB") md5=${DB_BEFORE:0:16}"
echo

echo "== the database itself: every access shape must be refused =="
d0 "read"            /bin/cat   $DB
d0 "write (truncate)" /bin/sh -c "echo pwned > $DB"
d0 "append"          /bin/sh -c "echo pwned >> $DB"
d0 "truncate (':>')" /bin/sh -c ":> $DB"
d0 "unlink"          /bin/sh -c "unlink $DB"
d0 "rm -f"           /bin/rm -f $DB
d0 "rename away"     /bin/mv $DB /t/stolen
d0 "rename onto it"  /bin/sh -c "echo x > /t/f; mv /t/f $DB"
d0 "chmod"           /bin/chmod 777 $DB
d0 "chown"           /bin/chown 4242:4242 $DB
d0 "read via /./ "   /bin/cat   /./.uvroot-vperm
d0 "read via /t/.."  /bin/cat   /t/../.uvroot-vperm

echo "== through a symlink to it =="
a0 /bin/ln -s $DB /t/dbsym >/dev/null 2>&1
ok   "creating a symlink to it is harmless" "$(test -L "$R/t/dbsym" && echo yes)" "yes"
d0 "read via symlink"  /bin/cat /t/dbsym
d0 "write via symlink" /bin/sh -c "echo pwned > /t/dbsym"

echo "== through a hard link to it (the fixed bypass) =="
a0 /bin/ln $DB /t/dblink >/dev/null 2>&1
ok   "a hard link can be made" "$(test -f "$R/t/dblink" && echo yes)" "yes"
d0 "read via hardlink"  /bin/cat /t/dblink
d0 "write via hardlink" /bin/sh -c "echo 999999 0 0 pwned >> /t/dblink"
d0 "chmod via hardlink" /bin/chmod 666 /t/dblink
d0 "rm via hardlink"    /bin/rm -f /t/dblink

echo "== invariants after the attacks =="
ok "database content unchanged" "$(md5sum "$HOSTDB" | cut -c1-32)" "$DB_BEFORE"
ok "no forged entry got in"     "$(grep -c 'pwned' "$HOSTDB")" "0"
ok "database still exists"      "$(test -f "$HOSTDB" && echo yes)" "yes"
ok "host mode unchanged"        "$(stat -c %a "$HOSTDB")" "600"
ok "no stray /t/stolen"         "$(test -e "$R/t/stolen" && echo yes || echo no)" "no"
ok "uvroot still enforces"      "$(PATH=$GP $U -r "$R" --vperm --vperm-id=1000:1000 /bin/cat /t/data.txt 2>&1 | grep -c 'Permission denied')" "1"

echo "== the fixed .tmp symlink attack =="
rm -f "$HOSTDB"; g --vperm /bin/sh -c 'chmod 700 /t/data.txt' >/dev/null
printf 'IMPORTANT-DATA\n' > "$R/t/victim"
a0 /bin/ln -s "$R/t/victim" /.uvroot-vperm.tmp >/dev/null 2>&1
ok "the symlink was planted" "$(test -L "$R/.uvroot-vperm.tmp" && echo yes)" "yes"
g --vperm /bin/sh -c 'chmod 705 /t/data.txt' >/dev/null   # forces a db_save()
ok "the host file was NOT overwritten" "$(head -1 "$R/t/victim")" "IMPORTANT-DATA"
ok "the planted .tmp is gone"          "$(test -e "$R/.uvroot-vperm.tmp" && echo yes || echo no)" "no"
ok "the save still happened"           "$(grep -c '100705' "$HOSTDB")" "1"

echo "== the legitimate writer #1: uvroot itself =="
INODE_BEFORE=$(stat -c %i "$HOSTDB")
g --vperm /bin/sh -c 'chown 1234:5678 /t/data.txt' >/dev/null
INODE_AFTER=$(stat -c %i "$HOSTDB")
ok "uvroot recorded the chown"      "$(g --vperm /bin/stat -c %u:%g /t/data.txt)" "1234:5678"
ok "the save replaced the inode"    "$([ "$INODE_BEFORE" != "$INODE_AFTER" ] && echo changed || echo same)" "changed"
# the freshly written database must be protected under its new inode too
a0 /bin/ln $DB /t/dblink2 >/dev/null 2>&1
d0 "new inode is protected as well" /bin/cat /t/dblink2
g --vperm /bin/mv /t/data.txt /t/moved.txt >/dev/null
ok "rename maintenance works"       "$(grep -c 'moved.txt' "$HOSTDB")" "1"

echo "== the legitimate writer #2: the host, from outside =="
printf '100741 4242 4242\tt/moved.txt\n' > "$HOSTDB"
ok "host edit is honoured" "$(g --vperm /bin/stat -c '%a %u:%g' /t/moved.txt)" "741 4242:4242"
ok "host can still read it" "$(test -r "$HOSTDB" && echo yes)" "yes"

echo
echo "db-protection: passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
