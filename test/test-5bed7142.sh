if [ ! -x  ${ROOTFS}/bin/pwd ] || [ -z `which mkdir` ] || [ -z `which grep` ] || [ -z `which mcookie` ] || [ -z `which pwd` ]; then
    exit 125;
fi

mkdir -p ${ROOTFS}/${PWD}
${UVROOT} -v 1 -w . -r ${ROOTFS} pwd | grep ^${PWD}$

TMP=/tmp/$(mcookie)
mkdir ${TMP}
! ${UVROOT} sh -c "cd ${TMP}; rmdir ${TMP}; $(which pwd) -P"
[ $? -eq 0 ]
