if [ ! -x ${ROOTFS}/bin/true ] || [ -z `which env` ]; then
    exit 125;
fi

! ${UVROOT} -r ${ROOTFS} /true
[ $? -eq 0 ]

! ${UVROOT} -r ${ROOTFS} ./true
[ $? -eq 0 ]

! env PATH='' ${UVROOT} -r ${ROOTFS} true
[ $? -eq 0 ]

! env PATH='' ${UVROOT} -r ${ROOTFS} -w /bin true
[ $? -eq 0 ]

env PATH='' ${UVROOT} -r ${ROOTFS} -w /bin ./true

env PATH='' ${UVROOT} -r ${ROOTFS} -w / bin/true

env PATH='' ${UVROOT} -r ${ROOTFS} -w / bin/./true

env PATH='' ${UVROOT} -r ${ROOTFS} -w / ../bin/true

! env PATH='' ${UVROOT} -r ${ROOTFS} -w /bin/true ../true
[ $? -eq 0 ]

! env --unset PATH ${UVROOT} -r ${ROOTFS} true
[ $? -eq 0 ]

! env --unset PATH ${UVROOT} -r ${ROOTFS} -w /bin true
[ $? -eq 0 ]

env --unset PATH ${UVROOT} -r ${ROOTFS} -w /bin ./true

env --unset PATH ${UVROOT} -r ${ROOTFS} -w / /bin/true

env --unset PATH ${UVROOT} -r ${ROOTFS} -w / /bin/./true

env --unset PATH ${UVROOT} -r ${ROOTFS} -w / ../bin/true

! env --unset PATH ${UVROOT} -r ${ROOTFS} -w /bin/true ../true
[ $? -eq 0 ]
