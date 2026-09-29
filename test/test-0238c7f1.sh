if [ ! -x  ${ROOTFS}/bin/pwd ] || [ -z `which grep` ]; then
    exit 125;
fi

${UVROOT} -m /:/hostfs -w /hostfs/etc -r ${ROOTFS} pwd | grep '^/hostfs/etc$'
${UVROOT} -m /:/hostfs -w /hostfs -r ${ROOTFS} pwd | grep '^/hostfs$'
${UVROOT} -m /:/hostfs -w / -r ${ROOTFS} pwd | grep '^/$'
