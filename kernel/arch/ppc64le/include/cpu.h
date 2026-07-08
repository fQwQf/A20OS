#ifndef _ARCH_PPC64LE_CPU_H
#define _ARCH_PPC64LE_CPU_H

#include "core/types.h"
#include "platform.h"

static inline void arch_mb(void)  { __asm__ __volatile__("sync" ::: "memory"); }
static inline void arch_rmb(void) { __asm__ __volatile__("lwsync" ::: "memory"); }
static inline void arch_wmb(void) { __asm__ __volatile__("lwsync" ::: "memory"); }
static inline void arch_wfi(void) { __asm__ __volatile__("or 27,27,27"); }
static inline void arch_cpu_relax(void) { __asm__ __volatile__("or 27,27,27"); }
static inline void arch_fence_i(void) {
    __asm__ __volatile__("sync\n\tisync" ::: "memory");
}

static inline unsigned arch_current_cpu_id(void) {
    return 0;
}

static inline uint64_t arch_read_msr(void) {
    uint64_t v;
    __asm__ __volatile__("mfmsr %0" : "=r"(v));
    return v;
}

static inline void arch_write_msr(uint64_t v) {
    __asm__ __volatile__("mtmsrd %0\n\tisync" :: "r"(v) : "memory");
}

static inline void arch_local_irq_disable(void) {
    arch_write_msr(arch_read_msr() & ~PPC64_MSR_EE);
}

static inline void arch_local_irq_enable(void) {
    arch_write_msr(arch_read_msr() | PPC64_MSR_EE);
}

static inline int arch_irqs_enabled(void) {
    return !!(arch_read_msr() & PPC64_MSR_EE);
}

static inline uint64_t arch_irq_save(void) {
    uint64_t flags = arch_read_msr();
    arch_local_irq_disable();
    return flags;
}

static inline void arch_irq_restore(uint64_t flags) {
    arch_write_msr(flags);
}

static inline void arch_irq_disable(void) {
    arch_local_irq_disable();
}

static inline void arch_irq_enable(void) {
    arch_local_irq_enable();
}

static inline int arch_local_irq_enabled(void) {
    return arch_irqs_enabled();
}

static inline void arch_tlb_flush(void) {
    __asm__ __volatile__("ptesync\n\ttlbsync\n\tptesync" ::: "memory");
}

static inline void arch_tlb_flush_page(uint64_t addr) {
    (void)addr;
    arch_tlb_flush();
}

static inline void arch_set_task_pointer(void *task) {
    __asm__ __volatile__("mr 13,%0" :: "r"(task));
}

static inline void *arch_get_task_pointer(void) {
    void *tp;
    __asm__ __volatile__("mr %0,13" : "=r"(tp));
    return tp;
}

static inline uint64_t arch_read_ra(void) {
    uint64_t ra;
    __asm__ __volatile__("mflr %0" : "=r"(ra));
    return ra;
}

static inline uint64_t arch_read_cycle(void) {
    uint32_t hi0, lo, hi1;
    do {
        __asm__ __volatile__("mfspr %0,269" : "=r"(hi0));
        __asm__ __volatile__("mfspr %0,268" : "=r"(lo));
        __asm__ __volatile__("mfspr %0,269" : "=r"(hi1));
    } while (hi0 != hi1);
    return ((uint64_t)hi0 << 32) | lo;
}

static inline uint64_t arch_read_cause(void) {
    return 0;
}

static inline uint64_t arch_read_epc(void) {
    uint64_t v;
    __asm__ __volatile__("mfspr %0,26" : "=r"(v));
    return v;
}

static inline uint64_t arch_read_tval(void) {
    return 0;
}

static inline void arch_write_epc(uint64_t v) {
    __asm__ __volatile__("mtspr 26,%0" :: "r"(v));
}

static inline void arch_write_tvec(uint64_t v) {
    (void)v;
}

static inline uint64_t arch_read_satp(void) {
    return 0;
}

static inline void arch_write_satp(uint64_t v) {
    (void)v;
}

static inline uint64_t arch_read_addr_space_token(void) { return arch_read_satp(); }
static inline void arch_write_addr_space_token(uint64_t v) { arch_write_satp(v); }
static inline uint64_t arch_read_sstatus(void) { return arch_read_msr(); }
static inline void arch_write_sstatus(uint64_t v) { arch_write_msr(v); }
static inline uint64_t arch_read_sie(void) { return arch_irqs_enabled() ? 1 : 0; }
static inline void arch_write_sie(uint64_t v) { if (v) arch_local_irq_enable(); else arch_local_irq_disable(); }
static inline uint64_t arch_read_sip(void) { return 0; }
static inline void arch_write_sip(uint64_t v) { (void)v; }
static inline uint64_t arch_read_sscratch(void) { return 0; }
static inline void arch_write_sscratch(uint64_t v) { (void)v; }

static inline void __attribute__((noreturn)) arch_halt(void) {
    arch_local_irq_disable();
    while (1)
        __asm__ __volatile__("or 27,27,27");
}

static inline int arch_is_kernel_address(const void *ptr) {
    return (uintptr_t)ptr >= PAGE_OFFSET;
}

static inline void arch_dma_sync_for_device(const void *addr, size_t size) {
    (void)addr;
    (void)size;
}

static inline void arch_dma_sync_for_cpu(const void *addr, size_t size) {
    (void)addr;
    (void)size;
}

struct backtrace_frame {
    uint64_t pc;
};

static inline int arch_unwind_frames(uint64_t fp,
                                     struct backtrace_frame *frames,
                                     int max_frames) {
    int n = 0;
    for (int i = 0; i < max_frames && fp; i++) {
        uint64_t *frame = (uint64_t *)fp;
        if (!arch_is_kernel_address(frame))
            break;
        uint64_t next_fp = frame[0];
        uint64_t lr = frame[2];
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
