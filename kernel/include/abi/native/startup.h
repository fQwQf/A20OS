/*
 * A20OS Native ABI — Startup info structure (kernel→user contract).
 * Design reference: docs/native-abi/07-startup.md §1
 */
#ifndef _ABI_NATIVE_STARTUP_H
#define _ABI_NATIVE_STARTUP_H

#include "abi/native/types.h"

typedef struct a20_start_info {
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

#endif
