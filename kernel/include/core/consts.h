#ifndef _CONSTS_H
#define _CONSTS_H

#include "core/types.h"

/* ---------- Generic page size (same on all supported archs) ---------- */
#ifndef PAGE_SIZE
#define PAGE_SIZE          4096UL
#endif
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
/* Initial stack pages, raised from 16 to 32 (128 KiB), to stop execve from
 * overflowing the stack when argv/envp are large.  LTP tests such as thp01 pass
 * big environments; 16 pages (64 KiB) is not enough, and running out makes
 * elf_setup_stack write to an unmapped address, after which stale page-table
 * entries get used as physical addresses and the kernel crashes. */
#define USER_STACK_INITIAL_PAGES 32
#define USER_STACK_MAX_SIZE      (8 * 1024 * 1024UL)

/* ---------- Limits ---------- */
#ifdef CONFIG_MCU
#define MAX_FILES          16
#ifdef CONFIG_STM32_QEMU
#define MAX_PATH_LEN       32
#else
#define MAX_PATH_LEN       64
#endif
#define MAX_NAME_LEN       32
#define MAX_GROUPS         8
#else
/* Descriptor ceiling per process, and the hard clamp on RLIMIT_NOFILE
 * (sys_proc.c).  A Minecraft JVM keeps roughly three thousand asset files open
 * at once while the resource reload runs, so 1024 left it stalling on EMFILE
 * well before the main menu.  Per-process cost scales with this:
 * files_struct_t embeds fd[MAX_FILES] plus cloexec[MAX_FILES], about 36 KiB per
 * table, which is why this is not raised further.  Code that walks a whole
 * table must not put a MAX_FILES-sized array on the 64 KiB kernel stack --
 * see fdtable_files_put() and fdtable_close_on_exec(). */
#define MAX_FILES          4096
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

/* User virtual-address layout lives in mm/user_layout.h.  It used to be a single
 * global set defined here, which quietly assumed every architecture keeps its
 * user space in the bottom 4 GiB; aarch64, x86_64 and LoongArch all have wider
 * virtual addresses than that.  Nothing about the values changed -- they are the
 * same addresses -- but the layout is now per-architecture and can differ. */
#include "mm/user_layout.h"

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
