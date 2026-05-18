/*
 * A20OS liba20c — exit wrapper.
 */
#include "../liba20rt/a20_syscall.h"

void exit(int code)
{
    a20_task_exit(code);
    for (;;) {}
}

void _exit(int code)
{
    a20_task_exit(code);
    for (;;) {}
}
