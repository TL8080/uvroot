if [ -z `which id` ] || [ -z `which uname` ] || [ -z `which grep` ]; then
    exit 125;
fi

! ${UVROOT} ${UVROOT_RAW} /bin/true
if [ $? -eq 0 ]; then
    exit 125;
fi

kver=$(uname -r)

${UVROOT} ${UVROOT_RAW} -0 id -u                    | grep ^0$
${UVROOT} ${UVROOT_RAW} -i 123:456 id -u            | grep ^123$
${UVROOT} ${UVROOT_RAW} -k $kver-3.33.333 uname -r  | grep ^.*-3\.33\.333$

${UVROOT} -0       ${UVROOT_RAW} id -u              | grep ^0$
${UVROOT} -i 123:456 ${UVROOT_RAW} id -u            | grep ^123$
${UVROOT} -k $kver-3.33.333 ${UVROOT_RAW} uname -r  | grep ^.*-3\.33\.333$

${UVROOT} -0 ${UVROOT_RAW} -k $kver-3.33.333 id -u     | grep ^0$
${UVROOT} -0 ${UVROOT_RAW} -k $kver-3.33.333 uname -r  | grep ^.*-3\.33\.333$

${UVROOT} -k $kver-3.33.333 ${UVROOT_RAW} -0 id -u     | grep ^0$
${UVROOT} -k $kver-3.33.333 ${UVROOT_RAW} -0 uname -r  | grep ^.*-3\.33\.333$

${UVROOT} -i 123:456 ${UVROOT_RAW} -k $kver-3.33.333 id -u | grep ^123$
${UVROOT} -k $kver-3.33.333 ${UVROOT_RAW} -i 123:456 id -u | grep ^123$
