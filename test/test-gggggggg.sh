if [ -z `which mcookie` ] || [ -z `which env` ] || [ -z `which mkdir` ] || [ -z `which rm` ] || [ ! -x ${ROOTFS}/bin/readdir ]; then
    exit 125;
fi

TMP=/tmp/$(mcookie)
mkdir ${TMP}

! env UVROOT_DONT_POLLUTE_ROOTFS=1 ${UVROOT} -b /bin:${TMP}/dont/create ${ROOTFS}/bin/readdir ${TMP} | grep -w dont
[ $? -eq 0 ]

env UVROOT_DONT_POLLUTE_ROOTFS=1 ${UVROOT} -b /bin:${TMP}/dont/create test -e ${TMP}/dont

${UVROOT} -b /bin:${TMP}/dont/create test -e ${TMP}/dont

! test -e ${TMP}/dont
[ $? -eq 0 ]

chmod +rx -R ${TMP}
rm -fr ${TMP}
