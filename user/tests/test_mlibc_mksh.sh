#!/bin/mlibc-mksh
# Exercise mksh running on mlibc (Native ABI): builtins and external native
# commands via the capability-safe task_clone continuation + execve.
echo "B1 builtin-ok"
/bin/mlibc-echo "B2 external-ok"
/bin/mlibc-seq 1 3
echo "MKSH_MLIBC: PASS"
