#ifndef _ARCH_X86_64_H
#define _ARCH_X86_64_H

#define ARCH_HAS_VDSO 1
/* CET shadow stack (map_shadow_stack(2)).  Architectures without it keep
 * Linux's arch-correct -ENOSYS from the shared syscall body. */
#define ARCH_HAS_SHADOW_STACK 1

#include "platform.h"
#include "console.h"
#include "cpu.h"
#include "page_table.h"
#include "trap_frame.h"
#include "firmware.h"

#endif
