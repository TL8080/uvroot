#!/bin/sh
#
# Virtual process ids: the first program gets --vpid=N, /proc shows the
# virtual pids only, and the host pid space is not reachable.

if [ ! -x "${UVROOT}" ]; then
    exit 125
fi

host_pid=$$

result=$("${UVROOT}" -r / --vpid=7 /bin/sh -c "
    echo pid=\$\$
    if cat /proc/${host_pid}/status >/dev/null 2>&1; then
	echo host-visible=1
    else
	echo host-hidden=1
    fi
    sleep 5 &
    child=\$!
    echo child=\$child
    grep -E '^(Pid|PPid):' /proc/\$child/status | tr '\\n' ' '
    echo
    echo listing=\$(ls /proc | grep -cE '^[0-9]+\$')
    kill \$child 2>/dev/null
    wait \$child 2>/dev/null
    echo done
" 2>&1)

echo "${result}"

case "${result}" in
*"pid=7"*) ;;
*) echo "the first program did not get the virtual pid" >&2; exit 1 ;;
esac
case "${result}" in
*"host-hidden=1"*) ;;
*) echo "a host pid is still visible from the container" >&2; exit 1 ;;
esac
case "${result}" in
*"child=7"*) echo "the child reused the initial pid" >&2; exit 1 ;;
*"child="*) ;;
*) echo "the child has no virtual pid" >&2; exit 1 ;;
esac
case "${result}" in
*"PPid:	7"*) ;;
*) echo "the status file does not report the virtual parent" >&2; exit 1 ;;
esac
case "${result}" in
*"listing=0"*) echo "the /proc listing is empty" >&2; exit 1 ;;
esac
case "${result}" in
*"done"*) ;;
*) echo "the container did not finish cleanly" >&2; exit 1 ;;
esac
