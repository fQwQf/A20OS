#!/bin/mlibc-mksh
# Exercise mksh running on mlibc (Native ABI): builtins, external native
# commands via the capability-safe task_clone continuation + execve, and
# pipeline data flow (fork+dup2+exec stdio inheritance over channels).
echo "B1 builtin-ok"
/bin/mlibc-echo "B2 external-ok"
/bin/mlibc-seq 1 3
/bin/mlibc-seq 7 9 | /bin/mlibc-cat
echo "B4 direct-rc=$?"
P2=$(printf 'a\nb\nc')
echo "B5 comsub:$P2"
P6=$(/bin/mlibc-seq 9 12)
echo "B6 rc=$? comsub-seq:${P6//$'\n'/+}"
P1=$(/bin/mlibc-seq 1 5 | /bin/mlibc-cat)
echo "B3 got:${P1//$'\n'/+}"
P7=$(/bin/mlibc-pipeexec-rv)
echo "B7 probe-rc=$?"
echo "MKSH_MLIBC: PASS"
