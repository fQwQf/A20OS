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

typedef struct {
    uint32_t size;
    uint32_t version;
    uint32_t argc;
    uint32_t envc;
    uint32_t auxc;
    uint32_t reserved0;
    uint64_t argv;
    uint64_t envp;
    uint64_t auxv;
    a20_handle_t root_dir;
    a20_handle_t cwd_dir;
    a20_handle_t stdin_handle;
    a20_handle_t stdout_handle;
    a20_handle_t stderr_handle;
    a20_handle_t self_task;
    a20_handle_t main_thread;
    a20_handle_t default_event_queue;
    uint64_t page_size;
    uint64_t user_clock_freq;
} a20_start_info_t;

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
