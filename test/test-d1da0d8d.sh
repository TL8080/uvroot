if [ ! -x  ${ROOTFS}/bin/readlink ] || [ ! -e /proc/self/cwd ] || [ -z `which grep` ]; then
    exit 125;
fi

${UVROOT} -m /proc -m /tmp:/asym -w /asym -r ${ROOTFS} /bin/readlink /proc/self/cwd | grep '^/asym$'
${UVROOT} -m /proc -m /tmp:/asym -w /tmp  -r ${ROOTFS} /bin/readlink /proc/self/cwd | grep '^/tmp$'
