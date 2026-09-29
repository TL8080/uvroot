if [ -z `which sh` ] || [ -z `which readlink` ] || [ -z `which grep` ] || [ -z `which echo` ]  || [ -z `which mcookie` ] || [ ! -e /proc/self/fd/0 ]; then
    exit 125;
fi

${UVROOT} readlink /proc/self | grep -E "^[[:digit:]]+$"

! ${UVROOT} readlink /proc/self/..
[ $? -eq 0 ]

${UVROOT} readlink /proc/self/../self | grep -E "^[[:digit:]]+$"

${UVROOT} sh -c 'echo "OK" | readlink /proc/self/fd/0' | grep -E "^pipe:\[[[:digit:]]+\]$"

! ${UVROOT} sh -c 'echo "OK" | readlink /proc/self/fd/0/'
[ $? -eq 0 ]

! ${UVROOT} sh -c 'echo "OK" | readlink /proc/self/fd/0/..'
[ $? -eq 0 ]

! ${UVROOT} sh -c 'echo "OK" | readlink /proc/self/fd/0/../0'
[ $? -eq 0 ]

${UVROOT} sh -c 'echo "echo OK" | sh /proc/self/fd/0' | grep ^OK$

TMP=/tmp/$(mcookie)
${UVROOT} sh -c "exec 6<>${TMP}; readlink /proc/self/fd/6" | grep ^${TMP}
rm -f ${TMP}
