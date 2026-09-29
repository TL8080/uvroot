if [ -z `which make` ]; then
    exit 125;
fi

${UVROOT} make -f ${PWD}/test-c6b77b77.mk
