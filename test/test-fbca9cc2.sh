if [ -z `which strace` ] ||  [ -z `which true` ] || [ -z `which grep` ] || [ -z `which wc` ]; then
    exit 125;
fi

${UVROOT} strace -e trace=execve true 2>&1 | grep '^execve.*= 0$'

RESULT=$(${UVROOT} strace -e trace=execve true 2>&1 | grep '^execve' | wc -l)
test "${RESULT}" = "1"
