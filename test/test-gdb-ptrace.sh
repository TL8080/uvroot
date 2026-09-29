if [ ! -x  ${ROOTFS}/bin/gdb-ptrace-test ] || [ ! -x  ${ROOTFS}/bin/gdb-ptrace-test-signal ]; then
    exit 125;
fi

${ROOTFS}/bin/gdb-ptrace-test
${ROOTFS}/bin/gdb-ptrace-test 1
${UVROOT} ${ROOTFS}/bin/gdb-ptrace-test
${UVROOT} ${ROOTFS}/bin/gdb-ptrace-test 1

${ROOTFS}/bin/gdb-ptrace-test-signal
${ROOTFS}/bin/gdb-ptrace-test-signal 1
${UVROOT} ${ROOTFS}/bin/gdb-ptrace-test-signal
${UVROOT} ${ROOTFS}/bin/gdb-ptrace-test-signal 1
