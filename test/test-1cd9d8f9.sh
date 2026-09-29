if ! `which pwd` -P || [ -z `which grep` ] ; then
    exit 125;
fi

${UVROOT} -w /tmp pwd -P | grep '^/tmp$'
