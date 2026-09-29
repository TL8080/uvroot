if ! `which pwd` -P; then
    exit 125;
fi

${UVROOT} pwd -P
