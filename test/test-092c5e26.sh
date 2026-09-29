#!/bin/sh
set -eu

if [ ! -x "${ROOTFS}/bin/echo" ] ||  \
   [ ! -x "${ROOTFS}/bin/argv0" ] || \
   [ -z "$(command -v mcookie)" ] || \
   [ -z "$(command -v grep)" ] || \
   [ -z "$(command -v cat)" ] || \
   [ -z "$(command -v rm)" ]; then
    exit 125;
fi

TMP="$(mcookie)"
TMP_ABS="/tmp/${TMP}"

"${UVROOT}" -r "${ROOTFS}" argv0 | grep '^argv0$'
"${UVROOT}" -r "${ROOTFS}" /bin/argv0 | grep '^/bin/argv0$'

cat > "${ROOTFS}/${TMP_ABS}" <<EOF
#!/bin/argv0
test
EOF

chmod +x "${ROOTFS}/${TMP_ABS}"

env PATH=/tmp "${UVROOT}" -r "${ROOTFS}" "${TMP}" | grep '^/bin/argv0$'
"${UVROOT}" -r "${ROOTFS}" "${TMP_ABS}" | grep '^/bin/argv0$'

# Valgrind uses LD_PRELOAD.
if echo "${UVROOT}" | grep -q valgrind; then
    EXTRA='-E LD_PRELOAD=.*'
else
    EXTRA="" # unbound variable
fi

unset LD_LIBRARY_PATH

# shellcheck disable=SC2046
env UVROOT_FORCE_FOREIGN_BINARY=1 PATH=/tmp:/bin:/usr/bin:$(dirname \
    "$(command -v echo)") "${UVROOT}" -r "${ROOTFS}" -q echo "${TMP}" \
    | grep "^-U LD_LIBRARY_PATH ${EXTRA}-0 /bin/argv0 /bin/argv0 ${TMP_ABS}$"

env UVROOT_FORCE_FOREIGN_BINARY=1 "${UVROOT}" -r "${ROOTFS}" -q echo "${TMP_ABS}" \
    | grep "^-U LD_LIBRARY_PATH ${EXTRA}-0 /bin/argv0 /bin/argv0 ${TMP_ABS}$"

cat > "${ROOTFS}/${TMP_ABS}" <<EOF
FOREIGN BINARY FORMAT
EOF

chmod +x "${ROOTFS}/${TMP_ABS}"

# Valgrind prepends "/bin/sh" in front of foreign binaries.
if ! echo "${UVROOT}" | grep -q valgrind; then
    # shellcheck disable=SC2046
    env PATH=/tmp:/bin:/usr/bin:$(dirname "$(command -v echo)") \
    "${UVROOT}" -r "${ROOTFS}" -q echo "${TMP}" \
    | grep "^-U LD_LIBRARY_PATH -0 ${TMP} ${TMP_ABS}$"

    "${UVROOT}" -r "${ROOTFS}" -q echo "${TMP_ABS}" \
    | grep "^-U LD_LIBRARY_PATH -0 ${TMP_ABS} ${TMP_ABS}$"
fi

rm -f "${TMP_ABS}"
