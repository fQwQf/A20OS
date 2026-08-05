/*
 * A20OS Native ABI — startup info re-export.
 * The kernel side of a20_start_info_t lives internally
 * (kernel/include/ipc/start_info.h).
 */
#ifndef _ABI_NATIVE_STARTUP_H
#define _ABI_NATIVE_STARTUP_H

#include "proc/proc.h"
#include "ipc/start_info.h"

int a20_prepare_start_info(task_t *task, const char *init_path,
                           uint64_t stack_top, uint64_t *out_sp);

#endif
