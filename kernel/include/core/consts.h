#ifndef _CONSTS_H
#define _CONSTS_H

#include "core/types.h"

/* ---------- Generic page size (same on all supported archs) ---------- */
#define PAGE_SIZE          4096UL
#define PAGE_SIZE_BITS     12
#define PAGE_OFFSET_MASK   ((1UL << PAGE_SIZE_BITS) - 1)
#define PMD_SHIFT          21
#define PMD_SIZE           (1UL << PMD_SHIFT)
#define PMD_ORDER          (PMD_SHIFT - PAGE_SIZE_BITS)
#define PMD_PAGE_COUNT     (PMD_SIZE / PAGE_SIZE)

/* ---------- Kernel / user stack sizes ---------- */
#define KERNEL_STACK_SIZE        (64 * 1024)
#define USER_STACK_INITIAL_PAGES 16
#define USER_STACK_MAX_SIZE      (8 * 1024 * 1024UL)

/* ---------- Limits ---------- */
#define MAX_PROCS          256
#define MAX_FILES          256
#define MAX_PATH_LEN       512
#define MAX_NAME_LEN       256
#define MAX_ARGS           256
#define MAX_ARG_STRLEN     (128 * 1024)
#define MAX_ARG_STRINGS    256
#define MAX_ARG_BYTES      (USER_STACK_MAX_SIZE / 4)
#define MAX_CMD_LEN        4096
#define MAX_HISTORY        256
#define MAX_GROUPS         32

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

/*
 * Compatibility aggregation for existing kernel code that still treats errno,
 * open flags, mmap flags and stat bits as shared constants. New ABI boundary
 * code should include abi/current.h or a specific ABI header directly.
 */
#include "abi/current.h"

#endif /* _CONSTS_H */
