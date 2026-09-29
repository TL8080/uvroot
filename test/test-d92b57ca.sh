if [ -z `which env` ] || [ -z `which true` ]; then
    exit 125;
fi

env UVROOT_NO_SUBRECONF=1 ${UVROOT} ${UVROOT} -v 1 true
