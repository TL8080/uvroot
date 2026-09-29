#!/bin/sh
# shellcheck disable=SC2086
set -eu

if [ -z "$(command -v mknod)" ] || \
   [ "$(id -u)" -eq 0 ]; then
    exit 125
fi

TMP="/tmp/$(mcookie)"

[ ! "$(${UVROOT} mknod ${TMP} b 1 1)" = "0" ]

[ ! "$(${UVROOT} -i 123:456 mknod ${TMP} b 1 1)" = "0" ]

"${UVROOT}" -0 mknod "${TMP}" b 1 1
