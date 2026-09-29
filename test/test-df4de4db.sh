if [ ! -x  ${ROOTFS}/bin/fork-wait ] || [ -z `which strace` ]; then
    exit 125;
fi

${UVROOT} strace ${ROOTFS}/bin/fork-wait
${UVROOT} strace ${ROOTFS}/bin/fork-wait 2

${UVROOT} strace -f ${ROOTFS}/bin/fork-wait
${UVROOT} strace -f ${ROOTFS}/bin/fork-wait 2

