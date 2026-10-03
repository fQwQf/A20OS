#ifndef _MM_USER_LAYOUT_H
#define _MM_USER_LAYOUT_H

/*
 * User virtual-address layout.
 *
 * These used to be defined inline in core/consts.h as a single global set.  That
 * was accurate for as long as every architecture placed its user space in the
 * bottom 4 GiB, and it stopped being accurate the moment any of them did not:
 * aarch64 and x86_64 have 48-bit virtual addresses, LoongArch can do LA57, and
 * a port that wanted to use them was silently limited to the low 4 GiB with no
 * place to say so.
 *
 * The layout is per-architecture now.  Every architecture currently resolves to
 * the same verified values -- moving one is a behaviour change that has to be
 * validated on that architecture, not a cosmetic edit -- but each can now differ,
 * and the place to change it is one header rather than a global that every
 * architecture shares by accident.
 *
 * ARCH_PT_USER_START / ARCH_PT_USER_END in each arch's page_table.h are the other
 * half of this contract: they are page-table indices bounding the user half of
 * the address space, while the constants here are the addresses handed out
 * inside it.  A 48-bit port changes both.
 */

#include "core/types.h"

/* mmap() picks addresses at or above this.  Above the dynamic loader's
 * INTERP_BASE_ADDR so a PIE interpreter and the libraries that follow it do not
 * collide with anonymous mappings. */
#ifndef USER_MMAP_BASE
#define USER_MMAP_BASE     0x60000000UL
#endif
#define MMAP_BASE_ADDR      USER_MMAP_BASE

/* First address a PIE interpreter is loaded at. */
#ifndef USER_INTERP_BASE
#define USER_INTERP_BASE    0x40000000UL
#endif
#define INTERP_BASE_ADDR    USER_INTERP_BASE

/* Lowest address a user thread's stack may reach.  Eight MiB of headroom below
 * the top is what an 8 MiB stack occupies; growth stops here rather than running
 * into the vDSO area pinned just below. */
#ifndef USER_STACK_LIMIT
#define USER_STACK_LIMIT    0x3FFFF000UL
#endif
#define USER_STACK_TOP      USER_STACK_LIMIT

/* Floor for stack growth and stack randomisation.  The vDSO and vvar pages sit
 * immediately below this (see mm/vdso_layout.h) and neither the stack VMA nor
 * stack growth may cross into them. */
#ifndef USER_STACK_FLOOR_ADDR
#define USER_STACK_FLOOR_ADDR 0x3F800000UL
#endif
#define USER_STACK_FLOOR    USER_STACK_FLOOR_ADDR

/* Thread-local storage block. */
#ifndef USER_TLS_ADDR
#define USER_TLS_ADDR       0x3E000000UL
#endif
#define USER_TLS_BASE       USER_TLS_ADDR

/* Base for the loader's own mappings when relocating a non-PIE executable. */
#ifndef USER_DYN_ADDR
#define USER_DYN_ADDR       0x10000UL
#endif
#define USER_DYN_BASE       USER_DYN_ADDR

/*
 * Not moved: A20_VDSO_VA and A20_VVAR_VA in mm/vdso_layout.h.
 *
 * kernel/include/mm/vdso_layout.h is deliberately preprocessor-only because the
 * user-space vDSO assembly images for four architectures include it, and a .S
 * file cannot pull in core/types.h.  That is the remaining coupling between the
 * vDSO placement and everything else here: a 48-bit port would have to move the
 * vDSO too, which means teaching the shared header to be assembly-safe.  Doing
 * that is a prerequisite for that port, not part of this one, so the two
 * addresses stay where they are and this comment is the note explaining why.
 */

#endif /* _MM_USER_LAYOUT_H */