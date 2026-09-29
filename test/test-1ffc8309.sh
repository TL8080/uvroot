if [ -z `which mcookie` ] || [ -z `which mkdir` ] || [ -z `which env` ] || [ -z `which uname` ] || [ -z `which rm` ]; then
    exit 125;
fi

TMP=/tmp/$(mcookie)

mkdir ${TMP}

env UVROOT_FORCE_KOMPAT=1 ${UVROOT} -k $(uname -r) rm -r ${TMP}
