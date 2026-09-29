if [ -z `which mcookie` ] || [ -z `which rmdir` ] || [ -z `which mkdir` ]; then
    exit 125;
fi

TMP=$(mcookie)
cd /tmp

${UVROOT} mkdir ./${TMP}
${UVROOT} rmdir ./${TMP}

${UVROOT} mkdir ${TMP}/
${UVROOT} rmdir ${TMP}/

${UVROOT} mkdir ./${TMP}/
${UVROOT} rmdir ./${TMP}/
