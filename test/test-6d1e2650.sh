if [ ! -x  ${ROOTFS}/bin/true ] || [ -z `which env` ]; then
    exit 125;
fi

! env PATH=/nib ${UVROOT} -r ${ROOTFS} true
[ $? -eq 0 ]

env PATH=/bin ${UVROOT} -r ${ROOTFS} true
