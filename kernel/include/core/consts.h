#ifndef _CONSTS_H
#define _CONSTS_H

#include "core/types.h"

/* ---------- Generic page size (same on all supported archs) ---------- */
#define PAGE_SIZE          4096UL
#define PAGE_SIZE_BITS     12
#define PAGE_OFFSET_MASK   ((1UL << PAGE_SIZE_BITS) - 1)
#ifndef PMD_SHIFT
#define PMD_SHIFT          21
#define PMD_SIZE           (1UL << PMD_SHIFT)
#define PMD_ORDER          (PMD_SHIFT - PAGE_SIZE_BITS)
#define PMD_PAGE_COUNT     (PMD_SIZE / PAGE_SIZE)
#endif

/* ---------- Kernel / user stack sizes ---------- */
#ifdef CONFIG_MCU
/* STM32VL QEMU exposes only 8 KiB; it runs one diagnostic task. */
#ifdef CONFIG_STM32_QEMU
#define KERNEL_STACK_SIZE        512
#else
/* The 64 KiB Xuanwu target keeps 2 KiB per MCU kernel thread. */
#define KERNEL_STACK_SIZE        2048
#endif
#else
#define KERNEL_STACK_SIZE        (64 * 1024)
#endif
/* 初始栈页数：从16增加到32（128KB），防止 execve 时参数/环境变量过大导致栈溢出。
 * thp01 等LTP测试有较大的环境变量，16页(64KB)不够用，会导致 elf_setup_stack
 * 写到未映射的地址，引发页表中的脏数据被当作物理地址使用而崩溃。 */
#define USER_STACK_INITIAL_PAGES 32
#define USER_STACK_MAX_SIZE      (8 * 1024 * 1024UL)

/* ---------- Limits ---------- */
#ifdef CONFIG_MCU
#define MAX_PROCS          32
#define MAX_FILES          16
#ifdef CONFIG_STM32_QEMU
#define MAX_PATH_LEN       32
#else
#define MAX_PATH_LEN       64
#endif
#define MAX_NAME_LEN       32
#define MAX_GROUPS         8
#else
#define MAX_PROCS          4096
#define MAX_FILES          1024
#define MAX_PATH_LEN       512
#define MAX_NAME_LEN       256
#define MAX_GROUPS         32
#endif
#define MAX_ARGS           256
#define MAX_ARG_STRLEN     (128 * 1024)
#define MAX_ARG_STRINGS    256
#define MAX_ARG_BYTES      (USER_STACK_MAX_SIZE / 4)
#define MAX_CMD_LEN        4096
#define MAX_HISTORY        256

/* ---------- Internal file and memory layout constants ---------- */
#define FT_REGULAR    1
#define FT_DIRECTORY  2
#define FT_CHAR_DEV   3
#define FT_BLOCK_DEV  4
#define FT_PIPE       5
#define FT_SYMLINK    6

#define EXT4_SUPER_MAGIC  0x4006

#define MMAP_BASE_ADDR    0x60000000UL
#define USER_STACK_TOP    0x3FFFF000UL
#define USER_DYN_BASE     0x10000UL
#define USER_TLS_BASE     0x3E000000UL
#define INTERP_BASE_ADDR  0x40000000UL

#define PIPE_BUF_SIZE 4096
#define FIRST_USER_FD 3

/* Kernel-internal compatibility constants. */
#include "core/errno.h"
#include "core/fcntl.h"
#include "core/mman.h"
#include "core/poll.h"
#include "core/signal_defs.h"
#include "core/stat.h"

#endif /* _CONSTS_H */
