/*
 * A20OS liba20c — exit wrapper with atexit support.
 */
#include "../liba20rt/a20_syscall.h"
#include "../liba20rt/a20_task.h"
#include <stdlib.h>

static void (*atexit_handlers[ATEXIT_MAX])(void);
static int atexit_count = 0;

int atexit(void (*func)(void))
{
    if (!func || atexit_count >= ATEXIT_MAX) return -1;
    atexit_handlers[atexit_count++] = func;
    return 0;
}

static void call_atexit_handlers(void)
{
    int i;
    for (i = atexit_count - 1; i >= 0; i--)
        atexit_handlers[i]();
}

void exit(int code)
{
    call_atexit_handlers();
    a20_task_exit(code);
    for (;;) {}
}

void _exit(int code)
{
    a20_task_exit(code);
    for (;;) {}
}
