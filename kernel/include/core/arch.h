/*
 * A20OS Architecture Abstraction Layer
 *
 * This header dispatches to the correct arch-specific headers based on
 * the CONFIG_* define set by the Makefile (-DCONFIG_RISCV64,
 * -DCONFIG_RISCV32, -DCONFIG_LOONGARCH64, -DCONFIG_AARCH64,
 * -DCONFIG_ARM32, -DCONFIG_ARMV7M, -DCONFIG_PPC64LE or -DCONFIG_X86_64).
 *
 * All arch-specific code (inline asm, register access, page table format,
 * trap context layout, hardware addresses) lives under kernel/arch/$(ARCH)/.
 * Shared kernel code includes only this header (or the individual
 * sub-headers it pulls in) and NEVER touches arch-specific registers or
 * instructions directly.
 */
#ifndef _ARCH_H
#define _ARCH_H

#include "core/types.h"

/*
 * Sub-headers provided by each architecture:
 *   arch.h       — master include (pulls in everything below)
 *   cpu.h        — barriers, irq control, wfi, TLB flush, CSR/register access
 *   page_table.h — page table format (PTE flags, VPN/PPN macros, SATP/TTBR)
 *   trap_frame.h — trap_context_t, task_context_t, syscall register access
 *   platform.h   — HW base addresses, IRQ numbers, exception codes, PAGE_OFFSET
 *   firmware.h   — shutdown/reboot/console/timer firmware calls (SBI or equiv)
 */

#if defined(CONFIG_RISCV64)
# include "arch/riscv64/include/arch.h"
#elif defined(CONFIG_RISCV32)
# include "arch/riscv32/include/arch.h"
#elif defined(CONFIG_LOONGARCH64)
# include "arch/loongarch64/include/arch.h"
#elif defined(CONFIG_LOONGARCH32)
# include "arch/loongarch32/include/arch.h"
#elif defined(CONFIG_AARCH64)
# include "arch/aarch64/include/arch.h"
#elif defined(CONFIG_ARM32)
# include "arch/arm32/include/arch.h"
#elif defined(CONFIG_ARMV7M)
# include "arch/armv7m/include/arch.h"
#elif defined(CONFIG_PPC64LE)
# include "arch/ppc64le/include/arch.h"
#elif defined(CONFIG_X86_64)
# include "arch/x86_64/include/arch.h"
#else
# error "No architecture defined. Set ARCH=riscv64, ARCH=riscv32, ARCH=loongarch64, ARCH=loongarch32, ARCH=aarch64, ARCH=arm32, ARCH=armv7m, ARCH=ppc64le or ARCH=x86_64."
#endif

/* Architecture-owned initialization hooks. The generic definitions provide
 * conservative fallbacks when an architecture does not override them.
 * init_kthread() calls the bootarg diagnostics once after scheduler startup. */
void arch_run_bootarg_selftests(void);
/* Physical byte address just past memory reserved before frame allocation. */
paddr_t arch_pfa_init_limit(void);
/* Reserved module allocator: 1=allocated, with *addr_out set to the load VA;
 * 0=use the frame pool; negative errno=allocation failure (out is undefined). */
int arch_drvmod_alloc_reserved(uint32_t order, uintptr_t *addr_out);
/* Release only a load VA returned by a prior successful reserved allocation. */
void arch_drvmod_free_reserved(uintptr_t addr, uint32_t order);

/* Keep the conservative global behavior on architectures without a
 * local-only full TLB flush primitive. */
#ifndef ARCH_HAS_LOCAL_TLB_FLUSH
# define arch_tlb_flush_local() arch_tlb_flush()
#endif

/* Architectures without tagged TLBs retain the conservative full-flush
 * behavior.  Tagged architectures override both hooks in their cpu header. */
#ifndef ARCH_HAS_LOCAL_ASID_TLB_FLUSH
static inline void arch_tlb_flush_asid_local(uint64_t asid)
{
    (void)asid;
    arch_tlb_flush_local();
}
#endif

#ifndef ARCH_HAS_ADDR_SPACE_SWITCH
static inline void arch_switch_addr_space_token(uint64_t token)
{
    if (arch_read_addr_space_token() == token)
        return;
    arch_write_addr_space_token(token);
    arch_tlb_flush_local();
}
#endif

/* Optional tagged-address-space lifecycle hooks. Architectures with ASIDs
 * override these macros in their master arch header; all others keep the
 * historical untagged address-space token and full-flush behavior. */
#ifndef ARCH_MM_CONTEXT_ALLOC
# define ARCH_MM_CONTEXT_ALLOC() 0U
#endif
#ifndef ARCH_MM_CONTEXT_RELEASE
# define ARCH_MM_CONTEXT_RELEASE(context_id) do { (void)(context_id); } while (0)
#endif
#ifndef ARCH_MM_ADDRESS_SPACE_TOKEN
# define ARCH_MM_ADDRESS_SPACE_TOKEN(pgdir, context_id) \
    ((void)(context_id), arch_make_addr_space_token(pgdir))
#endif

static inline uint32_t arch_mm_context_alloc(void)
{
    return ARCH_MM_CONTEXT_ALLOC();
}

static inline void arch_mm_context_release(uint32_t context_id)
{
    ARCH_MM_CONTEXT_RELEASE(context_id);
}

static inline uint64_t arch_mm_address_space_token(void *pgdir,
                                                    uint32_t context_id)
{
    return ARCH_MM_ADDRESS_SPACE_TOKEN(pgdir, context_id);
}

/*
 * Optional architecture capabilities.  Architecture headers opt in by
 * defining the corresponding ARCH_* macro; shared scheduler/process code
 * consumes only these hooks and does not branch on CONFIG_<architecture>.
 */
#ifndef ARCH_TASK_CONTEXT_SET_USER_TP
# define ARCH_TASK_CONTEXT_SET_USER_TP(ctx, user_tp) \
    do { (void)(ctx); (void)(user_tp); } while (0)
#endif

/* A fork-like operation may need to copy architectural user FP state into
 * the child's initial context.  Fresh tasks retain the architecture's normal
 * initial state; architectures that support live FP inheritance opt in. */
#ifndef ARCH_TASK_CONTEXT_COPY_USER_FP
# define ARCH_TASK_CONTEXT_COPY_USER_FP(ctx) do { (void)(ctx); } while (0)
#endif

#ifndef ARCH_PT_LEVEL_ENTRIES
# define ARCH_PT_LEVEL_ENTRIES(level) ARCH_PT_ENTRIES
#endif

#ifndef ARCH_PT_ROOT_ORDER
# define ARCH_PT_ROOT_ORDER 0
#endif

static inline int arch_pt_level_entries(int level)
{
    (void)level;
    return ARCH_PT_LEVEL_ENTRIES(level);
}

#ifndef ARCH_FORK_REQUIRES_PRIVATE_COPY
# define ARCH_FORK_REQUIRES_PRIVATE_COPY 0
#endif

/* Architecture-specific Linux ELF AT_HWCAP value.  Capabilities must only be
 * advertised when the kernel and hardware provide the corresponding ABI. */
#ifndef ARCH_ELF_HWCAP
# define ARCH_ELF_HWCAP() 0UL
#endif

#ifndef ARCH_TASK_FIELDS
# define ARCH_TASK_FIELDS
#endif

#ifndef ARCH_TASK_INIT
# define ARCH_TASK_INIT(task) do { (void)(task); } while (0)
#endif

#ifndef ARCH_SCHED_ENTER
# define ARCH_SCHED_ENTER(task) do { (void)(task); } while (0)
#endif

#ifndef ARCH_SCHED_LEAVE
# define ARCH_SCHED_LEAVE(task) do { (void)(task); } while (0)
#endif

#ifndef ARCH_SCHED_SWITCH
# define ARCH_SCHED_SWITCH(task) do { (void)(task); } while (0)
#endif

/* A safe idle wait is entered with local IRQs disabled and returns with them
 * still disabled. Architectures opt in only when their wakeup protocol closes
 * the interrupt-enable-to-sleep lost-wakeup window. */
#ifndef ARCH_HAS_SAFE_IDLE_WAIT
# define ARCH_HAS_SAFE_IDLE_WAIT 0
#endif

/* CET shadow stack (map_shadow_stack(2)).  Only x86_64 advertises it. */
#ifndef ARCH_HAS_SHADOW_STACK
# define ARCH_HAS_SHADOW_STACK 0
#endif
/*
 * Capability macros for behaviour that was previously an architecture-name test.
 *
 * Each of these replaces a CONFIG_<architecture> test in a file that has no
 * business knowing which architecture it is, and each names the *property* that
 * made the test necessary.  The real distinction in every case was a hardware
 * or firmware trait, not the instruction set: adding an architecture to the tree
 * silently changed behaviour nobody had decided on.
 *
 * All default to the safe or common case, so an architecture that does nothing
 * special gets the conservative answer.
 */

/*
 * The arch cannot reliably retain a private executable leaf pointing at a
 * page-cache frame: direct exec leaves lose their text PTE, which shows up as
 * loader SIGSEGVs.  x86_64 and loongarch64 both have this, and the fault.c
 * comment said so while only testing x86_64.
 */
#ifndef ARCH_EXE_LEAF_RETAIN_UNSAFE
# define ARCH_EXE_LEAF_RETAIN_UNSAFE 0
#endif

/*
 * Page-cache fault-around (mapping a whole window on one miss) is unsafe here
 * because a mapped-but-not-yet-uptodate window corrupts the dynamic symbols of
 * a shared object under parallel load.  loongarch64.
 */
#ifndef ARCH_FAULT_AROUND_UNSAFE
# define ARCH_FAULT_AROUND_UNSAFE 0
#endif

/*
 * An instruction storage fault loses the faulting EA, so demand paging can never
 * map the page and executable file pages must be mapped eagerly.  ppc64le, whose
 * external-vector entry clobbers SRR0.
 */
#ifndef ARCH_INSN_FAULT_UNRECOVERABLE
# define ARCH_INSN_FAULT_UNRECOVERABLE 0
#endif

/*
 * The timebase frequency is only known at runtime, from the firmware device
 * tree.  riscv64: QEMU virt publishes 10 MHz and StarFive JH7110 24 MHz, so a
 * compile-time constant is wrong on one of them.
 */
#ifndef ARCH_TIMER_FREQ_RUNTIME
# define ARCH_TIMER_FREQ_RUNTIME 0
#endif

/*
 * The current task is reachable from a single cheap register read rather than a
 * lookup.  riscv64 keeps it in tp.
 */
#ifndef ARCH_FAST_CURRENT
# define ARCH_FAST_CURRENT 0
#endif

/*
 * Hardware single-step exists.  riscv64 has none, which is why ptrace
 * single-step returns -EIO there -- same as Linux.
 */
#ifndef ARCH_HW_SINGLE_STEP
# define ARCH_HW_SINGLE_STEP 0
#endif

/*
 * The Linux epoll_event wire struct is packed on this arch, which the ABI layer
 * has to reproduce exactly.
 */
#ifndef ARCH_LINUX_EPOLL_EVENT_PACKED
# define ARCH_LINUX_EPOLL_EVENT_PACKED 0
#endif

#ifndef ARCH_IDLE_CONTEXT_STATIC
# define ARCH_IDLE_CONTEXT_STATIC(name, count)
# define ARCH_IDLE_STACK(contexts, cpu) kmalloc(KERNEL_STACK_SIZE)
# define ARCH_IDLE_STACK_INIT(stack) memset((stack), 0, KERNEL_STACK_SIZE)
# define ARCH_IDLE_STACK_TOP(stack) ((uintptr_t)(stack) + KERNEL_STACK_SIZE)
#endif

static inline int arch_fork_requires_private_copy(void)
{
    return ARCH_FORK_REQUIRES_PRIVATE_COPY;
}

#ifndef ARCH_IS_USER_PAGE_PERMISSION_FAULT
# define ARCH_IS_USER_PAGE_PERMISSION_FAULT(code) ((void)(code), 0)
#endif

static inline int arch_is_user_page_permission_fault(uint64_t code)
{
    return ARCH_IS_USER_PAGE_PERMISSION_FAULT(code);
}

static inline void arch_task_context_set_user_tp(task_context_t *ctx,
                                                  uintptr_t user_tp)
{
    ARCH_TASK_CONTEXT_SET_USER_TP(ctx, user_tp);
}

static inline void arch_task_context_copy_user_fp(task_context_t *ctx)
{
    ARCH_TASK_CONTEXT_COPY_USER_FP(ctx);
}

#ifndef ARCH_TASK_USER_RESUME_STATUS
# define ARCH_TASK_USER_RESUME_STATUS() arch_user_initial_status()
#endif

static inline uint64_t arch_task_user_resume_status(void)
{
    return ARCH_TASK_USER_RESUME_STATUS();
}

static inline void arch_syscall_dispatch_enter(void)
{
#ifndef ARCH_SYSCALL_DISPATCH_NONPREEMPTIBLE
    arch_local_irq_enable();
#endif
}

static inline void arch_syscall_dispatch_leave(void)
{
#ifndef ARCH_SYSCALL_DISPATCH_NONPREEMPTIBLE
    arch_local_irq_disable();
#endif
}

static inline int arch_syscall_resched_allowed(void)
{
#ifdef ARCH_SYSCALL_DISPATCH_NONPREEMPTIBLE
    return 0;
#else
    return 1;
#endif
}

#ifndef ARCH_TRAP_FAST_RETURN_ARM
# define ARCH_TRAP_FAST_RETURN_ARM(ctx) do { (void)(ctx); } while (0)
#endif
#ifndef ARCH_TRAP_FAST_RETURN_DISARM
# define ARCH_TRAP_FAST_RETURN_DISARM(ctx) do { (void)(ctx); } while (0)
#endif

/*
 * Was local interrupt delivery enabled in the context the trap interrupted?
 * The kernel preemption decision point (core/trap.c) only switches out when
 * this is true.  There is deliberately NO weak default here: a default of
 * "never enabled" would let an arch that forgot its hook compile with
 * CONFIG_KERNEL_PREEMPT on and then silently never preempt -- the config
 * would be a lie no build or boot would expose.  Every hosted arch defines
 * the macro against the status register its trap frame saved (the value the
 * trap entry pushed, not the post-entry state); an arch that genuinely
 * cannot report it has to say so by leaving CONFIG_KERNEL_PREEMPT off, and
 * core/trap.c turns a missing definition under the config on into a compile
 * error rather than a quiet zero.  Expands to an int expression, not a
 * statement.
 */

/* Arch name string (for uname, procfs, etc.) */
#if defined(CONFIG_RISCV64)
# define ARCH_NAME "riscv64"
#elif defined(CONFIG_RISCV32)
# define ARCH_NAME "riscv32"
#elif defined(CONFIG_LOONGARCH64)
# define ARCH_NAME "loongarch64"
#elif defined(CONFIG_LOONGARCH32)
# define ARCH_NAME "loongarch32"
#elif defined(CONFIG_AARCH64)
# define ARCH_NAME "aarch64"
#elif defined(CONFIG_ARM32)
# define ARCH_NAME "arm32"
#elif defined(CONFIG_ARMV7M)
# define ARCH_NAME "armv7m"
#elif defined(CONFIG_PPC64LE)
# define ARCH_NAME "ppc64le"
#elif defined(CONFIG_X86_64)
# define ARCH_NAME "x86_64"
#endif

/* Optional arch hook used by the ELF loader for dynamic-linker fallbacks. */
int arch_resolve_interp_fallback(const char *exec_path, const char *interp_path,
                                 char *resolved, size_t resolved_size);

/*
 * Optional architecture register dumps, for bring-up on a board whose firmware
 * hands control over in a state nobody has characterised yet.  Both are called
 * with interrupts off and before generic code starts printing, so an
 * implementation may read CSRs freely.  Printing is left to the implementation
 * because only the architecture knows what its registers are called.
 *
 * An architecture opts in by defining ARCH_HAS_BOOT_TRACE_DUMPS in its
 * arch/<arch>/include/arch.h and providing the two functions.  Everyone else
 * gets the no-op inline below, which is why most architectures print no
 * register line at all rather than an empty one.
 */
#ifdef ARCH_HAS_BOOT_TRACE_DUMPS
void arch_panic_dump(void);
void arch_debug_dump_user_state(void);
#else
static inline void arch_panic_dump(void) { }
static inline void arch_debug_dump_user_state(void) { }
#endif

/*
 * Optional hardware entropy source.  Returns 1 and writes a fresh 64-bit
 * hardware random value to *out on success, or 0 if the platform has no
 * usable hardware RNG.  The core RNG mixes this into the software PRNG state;
 * architectures without a hardware RNG use the weak default (returns 0).
 */
int arch_hw_entropy_sample(uint64_t *out);
#endif /* _ARCH_H */
