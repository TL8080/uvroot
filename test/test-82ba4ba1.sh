if [ ! -x /bin/true ] || [ -z `which id` ] || [ -z `which grep` ] || [ -z `which env` ] || [ -z `which chown` ] || [ -z `which chroot` ]; then
    exit 125;
fi

if [ `id -u` -eq 0 ]; then
    exit 125;
fi

${UVROOT} -i 123:456 id -u | grep ^123$
${UVROOT} -i 123:456 id -g | grep ^456$

${UVROOT} -i 123:456 env LD_SHOW_AUXV=1 /bin/true | grep '^AT_UID:[[:space:]]*123$'
${UVROOT} -i 123:456 env LD_SHOW_AUXV=1 /bin/true | grep '^AT_EUID:[[:space:]]*123$'
${UVROOT} -i 123:456 env LD_SHOW_AUXV=1 /bin/true | grep '^AT_GID:[[:space:]]*456$'
${UVROOT} -i 123:456 env LD_SHOW_AUXV=1 /bin/true | grep '^AT_EGID:[[:space:]]*456$'

! ${UVROOT} -i 123:456 chown root.root /root
[ $? -eq 0 ]

! chroot / /bin/true
EXPECTED=$?

! ${UVROOT} -i 123:456 chroot / /bin/true
[ $? -eq ${EXPECTED} ]

! ${UVROOT} -i 123:456 chroot /tmp/.. /bin/true
[ $? -eq ${EXPECTED} ]

! ${UVROOT} -i 123:456 chroot /tmp /bin/true
[ $? -eq 0 ]

${UVROOT} -0 id -u | grep ^0$
${UVROOT} -0 id -g | grep ^0$
${UVROOT} -0 chown root.root /root
${UVROOT} -0 chroot / /bin/true
${UVROOT} -0 chroot /tmp/.. /bin/true

${UVROOT} -0 env LD_SHOW_AUXV=1 /bin/true | grep '^AT_UID:[[:space:]]*0$'
${UVROOT} -0 env LD_SHOW_AUXV=1 /bin/true | grep '^AT_EUID:[[:space:]]*0$'
${UVROOT} -0 env LD_SHOW_AUXV=1 /bin/true | grep '^AT_GID:[[:space:]]*0$'
${UVROOT} -0 env LD_SHOW_AUXV=1 /bin/true | grep '^AT_EGID:[[:space:]]*0$'
