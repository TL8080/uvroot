if [ -z `which mcookie` ] || [ -z `which true` ] || [ -z `which mkdir` ]|| [ -z `which env` ]; then
    exit 125;
fi

TMP=/tmp/$(mcookie)
mkdir -p ${TMP}/true

! ${UVROOT} true
if [ $? -eq 0 ]; then
    exit 125;
fi

env PATH=${TMP}:${PATH} ${UVROOT} true

env PATH=${TMP}:${PATH} ${UVROOT} env true

rm -fr ${TMP}

