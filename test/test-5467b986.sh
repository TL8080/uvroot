if [ -z $(which mcookie) ] || [ -z $(which grep) ] || [ ! -x ${ROOTFS}/bin/readlink ] || [ ! -x ${ROOTFS}/bin/chdir_getcwd ] || [ ! -x ${ROOTFS}/bin/fchdir_getcwd ]; then
    exit 125;
fi

DOES_NOT_EXIST=/$(mcookie)
${UVROOT} -v -1 -b /proc -w ${DOES_NOT_EXIST} -r ${ROOTFS} readlink /proc/self/cwd | grep '^/$'

${UVROOT} -v -1 -w /a -b /tmp:/a -b /tmp:/b -r ${ROOTFS} pwd | grep '^/a$'
${UVROOT} -v -1 -w /a -b /tmp:/b -b /tmp:/a -r ${ROOTFS} pwd | grep '^/a$'
${UVROOT} -v -1 -w /b -b /tmp:/a -b /tmp:/b -r ${ROOTFS} pwd | grep '^/b$'
${UVROOT} -v -1 -w /b -b /tmp:/b -b /tmp:/a -r ${ROOTFS} pwd | grep '^/b$'

${UVROOT} -v -1 -b /tmp:/a -b /tmp:/b -r ${ROOTFS} chdir_getcwd /a | grep '^/[ab]$'
${UVROOT} -v -1 -b /tmp:/b -b /tmp:/a -r ${ROOTFS} chdir_getcwd /a | grep '^/[ab]$'
${UVROOT} -v -1 -b /tmp:/a -b /tmp:/b -r ${ROOTFS} chdir_getcwd /b | grep '^/[ab]$'
${UVROOT} -v -1 -b /tmp:/b -b /tmp:/a -r ${ROOTFS} chdir_getcwd /b | grep '^/[ab]$'

${UVROOT} -v -1 -b /tmp:/a -b /tmp:/b -r ${ROOTFS} fchdir_getcwd /a | grep '^/[ab]$'
${UVROOT} -v -1 -b /tmp:/b -b /tmp:/a -r ${ROOTFS} fchdir_getcwd /a | grep '^/[ab]$'
${UVROOT} -v -1 -b /tmp:/a -b /tmp:/b -r ${ROOTFS} fchdir_getcwd /b | grep '^/[ab]$'
${UVROOT} -v -1 -b /tmp:/b -b /tmp:/a -r ${ROOTFS} fchdir_getcwd /b | grep '^/[ab]$'

! ${UVROOT} -r ${ROOTFS} chdir_getcwd /bin/true
[ $? -eq 0 ]
! ${UVROOT} -r ${ROOTFS} fchdir_getcwd /bin/true
[ $? -eq 0 ]

! ${UVROOT} -w /bin -r ${ROOTFS} chdir_getcwd true
[ $? -eq 0 ]
! ${UVROOT} -w /bin -r ${ROOTFS} fchdir_getcwd true
[ $? -eq 0 ]

${UVROOT} -v -1 -w /usr -r / ${ROOTFS}/bin/chdir_getcwd share  | grep '^/usr/share$'
${UVROOT} -v -1 -w /usr -r / ${ROOTFS}/bin/fchdir_getcwd share | grep '^/usr/share$'

(cd /; ${UVROOT} -v -1 -w usr -r / ${ROOTFS}/bin/chdir_getcwd share  | grep '^/usr/share$')
(cd /; ${UVROOT} -v -1 -w usr -r / ${ROOTFS}/bin/fchdir_getcwd share | grep '^/usr/share$')
