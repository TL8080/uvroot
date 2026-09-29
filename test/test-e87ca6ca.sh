if [ -z `which mcookie` ] || [ -z `which cp` ] || [ -z `which true` ] || [ -z `which setcap` ] || [ -z `which rm` ]; then
    exit 125;
fi

if [ `id -u` -eq 0 ]; then
    exit 125;
fi

TMP=/tmp/$(mcookie)

cp $(which true) ${TMP}

! ${UVROOT} -i 123:456 setcap cap_setuid+ep ${TMP}
[ $? -eq 0 ]

${UVROOT} -0 setcap cap_setuid+ep ${TMP}

rm -f ${TMP}
