if [ ! -x  ${ROOTFS}/bin/true ]; then
    exit 125;
fi

${UVROOT} -w /bin -r ${ROOTFS} ./true

