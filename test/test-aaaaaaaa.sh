if [ ! -x ${ROOTFS}/bin/true ] || [ -z `which id` ] || [ -z `which mcookie` ] || [ -z `which ln` ] || [ -z `which rm` ]; then
    exit 125;
fi

if [ `id -u` -eq 0 ]; then
    exit 125;
fi

DONT_EXIST=/$(mcookie)

${UVROOT} -r ${ROOTFS} true

! ${UVROOT} ${DONT_EXIST} true
[ $? -eq 0 ]

${UVROOT} -r ${ROOTFS} true
${UVROOT} -r /etc -r ${ROOTFS} true

! ${UVROOT} -r ${ROOTFS} -r ${DONT_EXIST} true
[ $? -eq 0 ]

! ${UVROOT} -r ${DONT_EXIST} ${ROOTFS} true
[ $? -eq 0 ]

! ${UVROOT} ${ROOTFS} -r ${ROOTFS} true
[ $? -eq 0 ]

! ${UVROOT} -v
[ $? -eq 0 ]

${UVROOT} -b /bin/true:${DONT_EXIST} ${DONT_EXIST}

! ${UVROOT} -r / -b /etc:/ true
[ $? -eq 0 ]

! ${UVROOT} -b /etc:/ true
[ $? -eq 0 ]

${UVROOT} -b /etc:/ -r / true

TMP1=/tmp/$(mcookie)
TMP2=/tmp/$(mcookie)

echo "${TMP1}" > ${TMP1}
echo "${TMP2}" > ${TMP2}

REGULAR=/tmp/$(mcookie)
SYMLINK_TO_REGULAR=/tmp/$(mcookie)
ln -s ${REGULAR} ${SYMLINK_TO_REGULAR}

${UVROOT} -v -1 -b ${TMP1}:${REGULAR} -b ${TMP2}:${SYMLINK_TO_REGULAR} cat ${REGULAR} | grep "^${TMP2}$"
${UVROOT} -v -1 -b ${TMP2}:${SYMLINK_TO_REGULAR} -b ${TMP1}:${REGULAR} cat ${REGULAR} | grep "^${TMP1}$"

${UVROOT} -v -1 -b ${TMP1}:${REGULAR} -b ${TMP2}:${SYMLINK_TO_REGULAR}! cat ${REGULAR} | grep "^${TMP1}$"
${UVROOT} -v -1 -b ${TMP1}:${REGULAR} -b ${TMP2}:${SYMLINK_TO_REGULAR}! cat ${SYMLINK_TO_REGULAR} | grep "^${TMP2}$"

${UVROOT} -v -1 -b ${TMP1}:${REGULAR}! -b ${TMP2}:${SYMLINK_TO_REGULAR}! cat ${REGULAR} | grep "^${TMP1}$"
${UVROOT} -v -1 -b ${TMP1}:${REGULAR}! -b ${TMP2}:${SYMLINK_TO_REGULAR}! cat ${SYMLINK_TO_REGULAR} | grep "^${TMP2}$"

${UVROOT} -v -1 -b ${TMP1}:${REGULAR} -b ${TMP2}:${SYMLINK_TO_REGULAR} cat ${SYMLINK_TO_REGULAR} | grep "^${TMP2}$"
${UVROOT} -v -1 -b ${TMP2}:${SYMLINK_TO_REGULAR} -b ${TMP1}:${REGULAR} cat ${SYMLINK_TO_REGULAR} | grep "^${TMP1}$"

rm -fr ${TMP1} ${TMP2} ${REGULAR} $SYMLINK_TO_REGULAR}
