if [ -z `which mcookie` ] || [ -z `which ln` ] ||  [ -z `which true` ] ||  [ -z `which rm` ]; then
    exit 125;
fi

TMP=$(mcookie)
ln -s  /proc/self/mounts ${TMP}
${UVROOT} -b ${TMP} true
rm ${TMP}

