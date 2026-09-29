#!/bin/sh
set -eu

if [ ! -x "/usr/bin/echo" ] || \
   [ -z "$(command -v mcookie)" ] || \
   [ -z "$(command -v chmod)" ] || \
   [ -z "$(command -v env)" ] || \
   [ -z "$(command -v rm)" ]; then
    exit 125;
fi

TMP="/tmp/$(mcookie)"

echo '#!/usr/bin/echo XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX' > "${TMP}"

chmod +x "${TMP}"

RESULT="$(${UVROOT} ${TMP})"
EXPECTED="$(${TMP})"

[ "${RESULT}" = "${EXPECTED}" ]

echo '#!//../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../usr/bin/echo XXXXXXXXX' > "${TMP}"

RESULT="$(${UVROOT} ${TMP})"
EXPECTED="$(${TMP})"

[ "${RESULT}" = "${EXPECTED}" ]

echo '#!/../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../../usr/bin/echo XXXXXXXXX' > "${TMP}"

if ${TMP}; then
    # Linux kernel 5.1-rc1 increases the shebang limit to 256
    [ "${RESULT}" = "XXXXXXXXX ${TMP}" ]
    ${UVROOT} ${TMP}
else
    [ "${RESULT}" = "${TMP}" ]
    ! ${UVROOT} ${TMP}
fi

echo '#!                                                                                                                                                                                                        ' > ${TMP}

${UVROOT} ${TMP}

echo '#!' > ${TMP}

${UVROOT} ${TMP}

/usr/bin/echo "#!${TMP}" > ${TMP}

env LANG=C ${UVROOT} ${TMP} 2>&1 | grep 'Too many levels of symbolic links'
[ $? -eq 0 ]

rm -f "${TMP}"
