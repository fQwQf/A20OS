/*
 * A20OS Native SDK — crt0 startup for native programs.
 *
 * Entry point for native ABI programs. Reads a20_start_info from
 * the stack (placed there by kernel startup protocol), then calls main().
 */
#include "a20_syscall.h"
#include "a20_types.h"
#include "a20_task.h"

#ifndef __ASM__

int main(int argc, char **argv, char **envp);

/*
 * Weak libc hooks.  liba20rt must not depend on liba20c, but programs that
 * link liba20c get stdio/fdtable startup before main() and atexit handlers
 * after main() returns through these override points.  Programs built on
 * liba20rt alone keep the weak no-ops.  A TU that defines the strong
 * versions (user/liba20c/stdio.c) sets A20_LIBC_OWNS_HOOKS so the weak
 * defaults are not redefined in the same object.
 */
#ifndef A20_LIBC_OWNS_HOOKS
__attribute__((weak)) void __a20_libc_init(void) {}
__attribute__((weak)) void __a20_exit_run_hooks(void) {}
#endif
void __a20_libc_init(void);
void __a20_exit_run_hooks(void);

static a20_start_info_t *__start_info;

a20_start_info_t *a20_get_start_info(void) { return __start_info; }

void _start_c(a20_start_info_t *si)
{
    __start_info = si;
    __a20_libc_init();
    int ret = main((int)si->argc, (char **)si->argv, (char **)si->envp);
    __a20_exit_run_hooks();
    a20_task_exit(ret);
}

#endif
