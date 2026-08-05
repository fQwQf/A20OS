/*
 * A20OS vDSO (virtual dynamic shared object) — trap-free time queries.
 *
 * Design reference: docs/hybrid-kernel/00-design.md §7 (Linux ABI
 * transparent benefit path).  The kernel maps a tiny read-only code page
 * plus one shared read-only data page (vvar) into every Linux-ABI task at
 * exec and advertises the code page via AT_SYSINFO_EHDR.  musl then serves
 * clock_gettime/gettimeofday from user space, reading the same time CSR
 * the kernel timekeeping uses, with a seqlock-protected realtime base.
 *
 * Currently implemented for riscv64; other architectures get stubs that
 * keep the syscall path (correct, just slower).
 */
#ifndef _MM_VDSO_H
#define _MM_VDSO_H

#include "core/types.h"

/* Fixed user virtual addresses (below the initial stack region, which
 * starts 32 pages under USER_STACK_TOP = 0x3FFFF000). */
#define A20_VDSO_VA   0x3FFC0000UL
#define A20_VVAR_VA   0x3FFC2000UL

/* Shared data page layout; offsets must match vdso.S. */
typedef struct a20_vvar {
    uint32_t seq;            /* seqlock: odd while the kernel updates */
    uint32_t _pad;
    uint64_t mult;           /* ns per cycle << 32 */
    uint64_t boot_cycles;    /* time CSR value at kernel boot */
    uint64_t rt_base_cyc;    /* realtime anchor: cycles */
    uint64_t rt_base_sec;    /* realtime anchor: seconds */
    uint64_t rt_base_nsec;   /* realtime anchor: nanoseconds */
} a20_vvar_t;

#ifdef CONFIG_RISCV64

struct mm_struct;
struct vm_area;

void     vdso_init(uint64_t boot_cycles, uint64_t timer_freq);
void     vdso_sync_realtime(uint64_t sec, uint64_t nsec, uint64_t base_cyc);
int      vdso_map_image(pt_root_t *pgdir, struct vm_area **list);
int      vdso_exec_map(struct mm_struct *mm);
int      vdso_fork_map(struct mm_struct *child_mm);
vaddr_t  vdso_auxv_ehdr(void);

#else /* !CONFIG_RISCV64 */

struct mm_struct;
struct vm_area;

static inline void vdso_init(uint64_t boot_cycles, uint64_t timer_freq)
{ (void)boot_cycles; (void)timer_freq; }
static inline void vdso_sync_realtime(uint64_t sec, uint64_t nsec, uint64_t base_cyc)
{ (void)sec; (void)nsec; (void)base_cyc; }
static inline int vdso_map_image(pt_root_t *pgdir, struct vm_area **list)
{ (void)pgdir; (void)list; return -1; }
static inline int vdso_exec_map(struct mm_struct *mm) { (void)mm; return 0; }
static inline int vdso_fork_map(struct mm_struct *child_mm) { (void)child_mm; return 0; }
static inline vaddr_t vdso_auxv_ehdr(void) { return 0; }

#endif

#endif
