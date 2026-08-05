/*
 * A20OS core — native task startup info (kernel side).
 *
 * The kernel fills a20_start_info_t onto a native task's initial stack
 * (proc/exec, mm/elf).  The user side keeps its own layout-compatible
 * copy in the SDK (user/liba20rt/a20_types.h).  This header is internal;
 * the ABI layer exposes the same layout through abi/native/startup.h.
 */
#ifndef _IPC_START_INFO_H
#define _IPC_START_INFO_H

#include "ipc/ipc.h"

typedef struct a20_start_info {
    uint32_t size;
    uint32_t version;

    uint32_t argc;
    uint32_t envc;
    uint32_t auxc;
    uint32_t reserved0; /* native spawn fd mapping limit; zero for exec */

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

    /* Well-known service-registry client endpoint (M3); NULL when the
     * registry is unavailable. */
    a20_handle_t service_registry;
    uint32_t _pad_registry;
} a20_start_info_t;

#endif /* _IPC_START_INFO_H */
