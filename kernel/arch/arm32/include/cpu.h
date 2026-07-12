#ifndef _ARCH_ARM32_CPU_H
#define _ARCH_ARM32_CPU_H

#include "core/types.h"
#include "platform.h"

extern volatile uint32_t arm32_trap_flags;
uint32_t arm32_gic_ack(void);

/*
 * ARMv7 exception modes use banked stacks.  Until nested IRQ entry switches
 * to a dedicated re-entrant frame, keep syscall dispatch non-preemptible.
 */
#define ARCH_SYSCALL_DISPATCH_NONPREEMPTIBLE 1

static inline void arch_mb(void) { __asm__ __volatile__("dmb ish" ::: "memory"); }
static inline void arch_rmb(void) { __asm__ __volatile__("dmb ish" ::: "memory"); }
static inline void arch_wmb(void) { __asm__ __volatile__("dmb ishst" ::: "memory"); }
static inline void arch_wfi(void) { __asm__ __volatile__("wfi"); }
static inline void arch_cpu_relax(void) { __asm__ __volatile__("yield"); }
static inline void arch_fence_i(void) { __asm__ __volatile__("dsb ish\n\tisb" ::: "memory"); }
static inline void arch_flush_icache_range(const void *addr, size_t size) { (void)addr; (void)size; arch_fence_i(); }

static inline unsigned arch_current_cpu_id(void) {
    uint32_t mpidr;
    __asm__ __volatile__("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    return mpidr & 0x3U;
}

static inline uint32_t arch_read_cpsr(void) {
    uint32_t cpsr;
    __asm__ __volatile__("mrs %0, cpsr" : "=r"(cpsr));
    return cpsr;
}

static inline void arch_write_cpsr(uint32_t cpsr) {
    __asm__ __volatile__("msr cpsr_c, %0" :: "r"(cpsr) : "memory");
}

static inline void arch_local_irq_disable(void) {
    __asm__ __volatile__("cpsid i" ::: "memory");
}

static inline void arch_local_irq_enable(void) {
    __asm__ __volatile__("cpsie i" ::: "memory");
}

static inline int arch_irqs_enabled(void) {
    return (arch_read_cpsr() & (1U << 7)) == 0;
}

static inline uint32_t arch_irq_save(void) {
    uint32_t flags = arch_read_cpsr();
    arch_local_irq_disable();
    return flags;
}

static inline void arch_irq_restore(uint32_t flags) {
    arch_write_cpsr(flags);
}

static inline void arch_irq_disable(void) { arch_local_irq_disable(); }
static inline void arch_irq_enable(void) { arch_local_irq_enable(); }
static inline int arch_local_irq_enabled(void) { return arch_irqs_enabled(); }

static inline void arch_tlb_flush(void) {
#ifdef CONFIG_NOMMU
    arch_mb();
#else
    uint32_t zero = 0;
    __asm__ __volatile__(
        "dsb ishst\n\t"
        "mcr p15, 0, %0, c8, c7, 0\n\t"
        "dsb ish\n\t"
        "isb"
        :: "r"(zero)
        : "memory");
#endif
}

static inline void arch_tlb_flush_page(uint64_t addr) {
    (void)addr;
    arch_tlb_flush();
}

static inline void arch_set_task_pointer(void *task) {
    __asm__ __volatile__("mcr p15, 0, %0, c13, c0, 4" :: "r"(task));
}

static inline void *arch_get_task_pointer(void) {
    void *tp;
    __asm__ __volatile__("mrc p15, 0, %0, c13, c0, 4" : "=r"(tp));
    return tp;
}

static inline uint32_t arch_read_ra(void) {
    uint32_t ra;
    __asm__ __volatile__("mov %0, lr" : "=r"(ra));
    return ra;
}

static inline uint64_t arch_read_cycle(void) {
    uint32_t v;
    __asm__ __volatile__("mrc p15, 0, %0, c9, c13, 0" : "=r"(v));
    return v;
}

extern volatile uint32_t arm32_trap_flags;
extern volatile uint32_t arm32_fault_addr;
extern volatile uint32_t arm32_fault_pc;

static inline uint64_t arch_read_cause(void) { return (uint64_t)arm32_trap_flags; }
static inline uint64_t arch_read_epc(void) { return (uint64_t)arm32_fault_pc; }
static inline uint64_t arch_read_tval(void) { return (uint64_t)arm32_fault_addr; }
static inline void arch_write_epc(uint64_t v) { (void)v; }
static inline void arch_write_tvec(uint64_t v) {
    __asm__ __volatile__("mcr p15, 0, %0, c12, c0, 0\n\tisb" :: "r"((uint32_t)v) : "memory");
}

static inline uint64_t arch_read_satp(void) {
#ifdef CONFIG_NOMMU
    return 0;
#else
    uint32_t v;
    __asm__ __volatile__("mrc p15, 0, %0, c2, c0, 0" : "=r"(v));
    return v;
#endif
}

static inline void arch_write_satp(uint64_t v) {
#ifdef CONFIG_NOMMU
    (void)v;
    arch_mb();
#else
    uint32_t vv = (uint32_t)v;
    __asm__ __volatile__("mcr p15, 0, %0, c2, c0, 0\n\tdsb ish\n\tisb" :: "r"(vv) : "memory");
#endif
}

static inline uint64_t arch_read_addr_space_token(void) { return arch_read_satp(); }
static inline void arch_write_addr_space_token(uint64_t v) { arch_write_satp(v); }
static inline uint64_t arch_read_sstatus(void) { return arch_read_cpsr(); }
static inline void arch_write_sstatus(uint64_t v) { arch_write_cpsr((uint32_t)v); }
static inline uint64_t arch_read_sie(void) { return arch_irqs_enabled() ? 1U : 0U; }
static inline void arch_write_sie(uint64_t v) { if (v) arch_local_irq_enable(); else arch_local_irq_disable(); }
static inline uint64_t arch_read_sip(void) { return 0; }
static inline void arch_write_sip(uint64_t v) { (void)v; }
static inline uint64_t arch_read_sscratch(void) { return 0; }
static inline void arch_write_sscratch(uint64_t v) { (void)v; }

static inline void __attribute__((noreturn)) arch_halt(void) {
    arch_local_irq_disable();
    while (1)
        __asm__ __volatile__("wfi");
}

static inline int arch_is_kernel_address(const void *ptr) {
#ifdef CONFIG_NOMMU
    return (uintptr_t)ptr >= PHYS_MEMORY_BASE;
#else
    return (uintptr_t)ptr >= PAGE_OFFSET;
#endif
}

static inline void arch_dma_sync_for_device(const void *addr, size_t size) {
    (void)addr;
    (void)size;
    arch_mb();
}

static inline void arch_dma_sync_for_cpu(const void *addr, size_t size) {
    (void)addr;
    (void)size;
    arch_mb();
}

struct backtrace_frame {
    uint64_t pc;
};

static inline int arch_unwind_frames(uint64_t fp, struct backtrace_frame *frames, int max_frames) {
    int n = 0;
    while (n < max_frames && fp) {
        uint32_t *frame = (uint32_t *)(uintptr_t)fp;
        if (!arch_is_kernel_address(frame))
            break;
        uint32_t next_fp = frame[0];
        uint32_t lr = frame[-1];
        if (!lr)
            break;
        frames[n++].pc = lr;
        if (!next_fp || next_fp <= fp)
            break;
        fp = next_fp;
    }
    return n;
}

#endif
