#ifndef _ARCH_ARM32_H
#define _ARCH_ARM32_H

#define ARCH_NAME "arm32"

#include "platform.h"
#include "console.h"
#include "cpu.h"
#include "page_table.h"
#include "trap_frame.h"
#include "firmware.h"

#define ARCH_CACHE_LINE 64U

/* Offset trap.S stores the trap frame at inside task_t; proc.h asserts that
 * task_t.trap_ctx still lands there. */
#define ARCH_TASK_TRAP_CTX_OFFSET 68

#endif
