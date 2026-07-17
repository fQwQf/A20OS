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

static a20_start_info_t *__start_info;

a20_start_info_t *a20_get_start_info(void) { return __start_info; }

void _start_c(a20_start_info_t *si)
{
    __start_info = si;
    int ret = main((int)si->argc, (char **)si->argv, (char **)si->envp);
    a20_task_exit(ret);
}

#endif
