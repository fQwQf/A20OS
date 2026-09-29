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
 * The code page is per-architecture (kernel/vdso/<arch>/vdso.S) and is
 * assembled for aarch64, loongarch64, ppc64le, riscv64 and x86_64; every
 * other architecture takes the syscall path (correct, just slower).
 */
#ifndef _MM_VDSO_H
#define _MM_VDSO_H

#include "core/arch.h"
#include "mm/vdso_layout.h"

/*
 * getcpu refuses rather than guesses.
 *
 * __vdso_getcpu() in every kernel/vdso/<arch>/vdso.S returns -ENOSYS without
 * writing *cpu or *node.  -ENOSYS is the only refusal musl's wrapper knows:
 * src/sched/sched_getcpu.c treats any other non-zero result as a hard error,
 * but falls through to the getcpu syscall on -ENOSYS, and that syscall
 * (kernel/abi/linux/sys_sched.c) returns the true cpu_current_id().  The
 * caller therefore still gets the truth, via a trap it would have taken
 * anyway.
 *
 * A real fast path is not reachable from the vvar page as it exists today.
 * The kernel installs exactly one global vvar page (kernel/mm/vdso.c),
 * shared by every CPU and every task, so it cannot carry a per-CPU value;
 * A20OS keeps no per-CPU area pointer — per-CPU state is a plain array
 * indexed by the id (kernel/proc/current.c, kernel/proc/sched.c); and no
 * user-mode instruction on any of the five vDSO architectures yields a CPU
 * id, because each kernel backend reads a kernel-only register or field.
 * Each site documents the register its architecture actually uses.
 *
 * Storing 0 here, as this used to do, was the one unacceptable answer: it
 * reported a plausible, wrong CPU 0 on an SMP guest, where reporting "does
 * not exist" is honest (docs/security/hardening.md).
 *
 * Making it real needs a kernel-side export, not more assembly: a per-CPU
 * vvar page plus a way for user space to find the current one (a per-task
 * pointer in the thread area, the way Linux carries current_vdso), or on
 * x86_64 the per-CPU IA32_TSC_AUX MSR the kernel must program.  Keep this in
 * sync with docs/abi coverage notes if that ever happens.
 */

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

#ifdef ARCH_HAS_VDSO

struct mm_struct;
struct vm_area;

void     vdso_init(uint64_t boot_cycles, uint64_t timer_freq);
void     vdso_sync_realtime(uint64_t sec, uint64_t nsec, uint64_t base_cyc);
int      vdso_map_image(pt_root_t *pgdir, struct vm_area **list);
int      vdso_exec_map(struct mm_struct *mm);
int      vdso_fork_map(struct mm_struct *child_mm);
vaddr_t  vdso_auxv_ehdr(void);

#else /* !ARCH_HAS_VDSO */

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

/*
 * Raw free-running counter that user space can read for the vDSO, and its
 * frequency in Hz.  The weak defaults use the kernel tick counter, which is
 * what user space reads on riscv64/loongarch64/aarch64/ppc64le; x86_64
 * overrides them because user space reads the TSC while the kernel tick
 * counter is that TSC scaled to ARCH_TIMER_FREQ.  Declared for every arch
 * because timekeeping.c seeds its bases from them unconditionally.
 */
uint64_t arch_vdso_counter(void);
uint64_t arch_vdso_counter_freq(void);

#endif
