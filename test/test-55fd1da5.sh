if [ -z `which ls` ]; then
    exit 125;
fi

${UVROOT} -b /etc:/x ls -la /x
