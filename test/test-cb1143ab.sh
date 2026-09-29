if [ -z `which mcookie` ] || [ -z `which mkdir` ] ||  [ -z `which ln` ] ||  [ -z `which ls` ]; then
    exit 125;
fi

D1=`mcookie`
D2=`mcookie`
LINK=`mcookie`
F=`mcookie`
TMP=/tmp/${D1}/${D2}

mkdir -p ${TMP}
ln -s ${TMP}/./. ${TMP}/${LINK}

${UVROOT} \ls ${TMP}/${LINK} | grep ^${LINK}$
${UVROOT} \ls ${TMP}/${LINK}/ | grep ^${LINK}$
${UVROOT} \ls ${TMP}/${LINK}/. | grep ^${LINK}$
${UVROOT} \ls ${TMP}/${LINK}/.. | grep ^${D2}$
${UVROOT} \ls ${TMP}/${LINK}/./.. | grep ^${D2}$

rm ${TMP}/${LINK}
touch ${TMP}/${F}
ln -s ${TMP}/${F} ${TMP}/${LINK}

${UVROOT} \ls ${TMP}/${LINK}
! ${UVROOT} \ls ${TMP}/${LINK}/
[ $? -eq 0 ]

! ${UVROOT} \ls ${TMP}/${LINK}/.
[ $? -eq 0 ]

! ${UVROOT} \ls ${TMP}/${LINK}/..
[ $? -eq 0 ]

! ${UVROOT} \ls ${TMP}/${LINK}/./..
[ $? -eq 0 ]

! ${UVROOT} \ls ${TMP}/${LINK}/../..
[ $? -eq 0 ]

${UVROOT} -b /tmp/${D1}:${TMP}/${F} \ls ${TMP}/${LINK}
${UVROOT} -b /tmp/${D1}:${TMP}/${F} \ls ${TMP}/${LINK}/
${UVROOT} -b /tmp/${D1}:${TMP}/${F} \ls ${TMP}/${LINK}/.
${UVROOT} -b /tmp/${D1}:${TMP}/${F} \ls ${TMP}/${LINK}/..

rm ${TMP}/${LINK}
ln -s ${TMP}/${D1} ${TMP}/${LINK}

${UVROOT} -b /tmp/${F}:${TMP}/${D1} \ls ${TMP}/${LINK}
! ${UVROOT} -b /tmp/${F}:${TMP}/${D1} \ls ${TMP}/${LINK}/
[ $? -eq 0 ]

! ${UVROOT} -b /tmp/${F}:${TMP}/${D1} \ls ${TMP}/${LINK}/.
[ $? -eq 0 ]

! ${UVROOT} -b /tmp/${F}:${TMP}/${D1} \ls ${TMP}/${LINK}/..
[ $? -eq 0 ]

rm -fr /tmp/${D1}
