if [ ! -x /bin/sh ] || [ -z `which grep` ]; then
    exit 125;
fi

${UVROOT} -w /tmp /bin/sh -c 'echo $PWD' | grep '^/tmp$'
