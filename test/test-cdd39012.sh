if [ ! -x  ${ROOTFS}/bin/ptrace ] || [ ! -x  ${ROOTFS}/bin/ptrace-2 ] || [ ! -x  ${ROOTFS}/bin/true ]; then
    exit 125;
fi

${UVROOT} -r ${ROOTFS} ptrace
${UVROOT} -r ${ROOTFS} ptrace 2

${UVROOT} -r ${ROOTFS} ptrace-2 /bin/true

${UVROOT} -r ${ROOTFS} ptrace-2 /bin/fork-wait
${UVROOT} -r ${ROOTFS} ptrace-2 /bin/fork-wait 2
