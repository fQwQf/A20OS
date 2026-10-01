/*
 * A20OS liba20c — exit wrapper with atexit support.
 */
#include "../liba20rt/a20_syscall.h"
#include "../liba20rt/a20_task.h"
#include <stdlib.h>

extern void __a20_exit_run_hooks(void);

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
    __a20_exit_run_hooks();
    a20_task_exit(code);
    for (;;) {}
}

/* Strong override for crt0_a20.h's weak hook: atexit handlers run when main()
 * returns, not only on an explicit exit().  _exit() bypasses them by design. */
void __a20_exit_run_hooks(void)
{
    call_atexit_handlers();
}

void _exit(int code)
{
    a20_task_exit(code);
    for (;;) {}
}
