if ! `which pwd` -P || [ -z `which grep` ]; then
    exit 125;
fi

${UVROOT} pwd -P | grep "^$PWD$"
