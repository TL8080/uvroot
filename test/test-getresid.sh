if [ -z `which id` ] || [ -z `which grep` ] || [ ! -x ${ROOTFS}/bin/exec-suid ] || [ ! -x ${ROOTFS}/bin/exec-sgid ] || [ ! -x ${ROOTFS}/bin/getresuid ]  || [ ! -x ${ROOTFS}/bin/getresgid ]; then
    exit 125
fi

MY_UID=$(id -u)
MY_GID=$(id -g)

${UVROOT} -i ${MY_UID}:${MY_GID} -R ${ROOTFS} exec-suid /bin/getresuid | grep "^${MY_UID} 0 0$"

${UVROOT} -i ${MY_UID}:${MY_GID} -R ${ROOTFS} exec-suid /bin/getresgid | grep "^${MY_GID} ${MY_GID} ${MY_GID}$"

${UVROOT} -i ${MY_UID}:${MY_GID} -R ${ROOTFS} exec-sgid /bin/getresuid | grep "^${MY_UID} ${MY_UID} ${MY_UID}$"

${UVROOT} -i ${MY_UID}:${MY_GID} -R ${ROOTFS} exec-sgid /bin/getresgid | grep "^${MY_GID} 0 0$"

${UVROOT} -R ${ROOTFS} exec-suid /bin/getresuid | grep "^${MY_UID} ${MY_UID} ${MY_UID}$"

${UVROOT} -R ${ROOTFS} exec-suid /bin/getresgid | grep "^${MY_GID} ${MY_GID} ${MY_GID}$"

${UVROOT} -R ${ROOTFS} exec-sgid /bin/getresuid | grep "^${MY_UID} ${MY_UID} ${MY_UID}$"

${UVROOT} -R ${ROOTFS} exec-sgid /bin/getresgid | grep "^${MY_GID} ${MY_GID} ${MY_GID}$"

