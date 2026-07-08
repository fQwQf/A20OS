#ifndef _ARCH_RISCV32_H
#define _ARCH_RISCV32_H

#define ARCH_NAME "riscv32"
#ifndef PAGE_SHIFT
#define PAGE_SHIFT 12
#endif
#ifndef PAGE_SIZE
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#endif
#ifndef PAGE_MASK
#define PAGE_MASK (~(PAGE_SIZE - 1UL))
#endif
#define ARCH_LITTLE_ENDIAN 1
#define ARCH_CACHE_LINE_SIZE 64

#include "platform.h"
#include "console.h"
#include "cpu.h"
#include "page_table.h"
#include "trap_frame.h"
#include "firmware.h"

#endif
