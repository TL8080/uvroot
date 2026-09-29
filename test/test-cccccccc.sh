if [ -z `which mcookie` ] || [ -z `which rmdir` ] || [ -z `which mkdir` ]; then
    exit 125;
fi

TMP=/tmp/$(mcookie)
mkdir ${TMP}

! ${UVROOT} rmdir ${TMP}/.
[ $? -eq 0 ]

! ${UVROOT} rmdir ${TMP}/./
[ $? -eq 0 ]

${UVROOT} rmdir ${TMP}
