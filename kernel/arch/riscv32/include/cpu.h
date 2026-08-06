#ifndef _ARCH_RISCV32_CPU_H
#define _ARCH_RISCV32_CPU_H

#include "core/types.h"
#include "platform.h"

static inline void arch_mb(void) { __asm__ __volatile__("fence iorw,iorw" ::: "memory"); }
static inline void arch_rmb(void) { __asm__ __volatile__("fence ir,ir" ::: "memory"); }
static inline void arch_wmb(void) { __asm__ __volatile__("fence ow,ow" ::: "memory"); }
static inline void arch_wfi(void) { __asm__ __volatile__("wfi"); }
static inline void arch_cpu_relax(void) { __asm__ __volatile__("nop"); }
static inline void arch_fence_i(void) { __asm__ __volatile__(".word 0x0000100f" ::: "memory"); }
static inline void arch_flush_icache_range(const void *addr, size_t size) { (void)addr; (void)size; arch_fence_i(); }

static inline unsigned arch_current_cpu_id(void) {
    extern uint32_t __boot_hart_id;
    return (unsigned)__boot_hart_id;
}

static inline void arch_local_irq_disable(void) {
    __asm__ __volatile__("csrc sstatus, %0" :: "r"(SSTATUS_SIE));
}

static inline void arch_local_irq_enable(void) {
    __asm__ __volatile__("csrs sstatus, %0" :: "r"(SSTATUS_SIE));
}

static inline int arch_irqs_enabled(void) {
    uint32_t s;
    __asm__ __volatile__("csrr %0, sstatus" : "=r"(s));
    return !!(s & SSTATUS_SIE);
}

static inline uint32_t arch_irq_save(void) {
    uint32_t flags;
    __asm__ __volatile__("csrr %0, sstatus" : "=r"(flags));
    arch_local_irq_disable();
    return flags;
}

static inline void arch_irq_restore(uint32_t flags) {
    __asm__ __volatile__("csrw sstatus, %0" :: "r"(flags));
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
    __asm__ __volatile__("sfence.vma" ::: "memory");
}

static inline void arch_tlb_flush_page(uint64_t addr) {
    __asm__ __volatile__("sfence.vma %0, zero" :: "r"((uint32_t)addr) : "memory");
}
static inline void arch_tlb_flush_page_local(uint64_t addr) {
    arch_tlb_flush_page(addr);
}

static inline void arch_set_task_pointer(void *task) {
    __asm__ __volatile__("mv tp, %0" :: "r"(task));
}

static inline void *arch_get_task_pointer(void) {
    void *tp;
    __asm__ __volatile__("mv %0, tp" : "=r"(tp));
    return tp;
}

static inline uint64_t arch_read_ra(void) {
    uint32_t ra;
    __asm__ __volatile__("mv %0, ra" : "=r"(ra));
    return ra;
}

static inline uint64_t arch_read_cycle(void) {
    uint32_t hi0, lo, hi1;
    do {
        __asm__ __volatile__("rdcycleh %0" : "=r"(hi0));
        __asm__ __volatile__("rdcycle %0" : "=r"(lo));
        __asm__ __volatile__("rdcycleh %0" : "=r"(hi1));
    } while (hi0 != hi1);
    return ((uint64_t)hi0 << 32) | lo;
}

static inline uint64_t arch_read_cause(void) {
    uint32_t v;
    __asm__ __volatile__("csrr %0, scause" : "=r"(v));
    return v;
}

static inline uint64_t arch_read_epc(void) {
    uint32_t v;
    __asm__ __volatile__("csrr %0, sepc" : "=r"(v));
    return v;
}

static inline uint64_t arch_read_tval(void) {
    uint32_t v;
    __asm__ __volatile__("csrr %0, stval" : "=r"(v));
    return v;
}

static inline void arch_write_epc(uint64_t v) {
    __asm__ __volatile__("csrw sepc, %0" :: "r"((uint32_t)v));
}

static inline void arch_write_tvec(uint64_t v) {
    __asm__ __volatile__("csrw stvec, %0" :: "r"((uint32_t)v));
}

static inline uint64_t arch_read_satp(void) {
    uint32_t v;
    __asm__ __volatile__("csrr %0, satp" : "=r"(v));
    return v;
}

static inline void arch_write_satp(uint64_t v) {
    __asm__ __volatile__("csrw satp, %0" :: "r"((uint32_t)v));
}

static inline uint64_t arch_read_addr_space_token(void) {
#ifdef CONFIG_NOMMU
    return 0;
#else
    return arch_read_satp();
#endif
}
static inline void arch_write_addr_space_token(uint64_t v) {
#ifdef CONFIG_NOMMU
    (void)v;
#else
    arch_write_satp(v);
#endif
}
static inline uint64_t arch_read_sstatus(void) { uint32_t v; __asm__ __volatile__("csrr %0, sstatus" : "=r"(v)); return v; }
static inline void arch_write_sstatus(uint64_t v) { __asm__ __volatile__("csrw sstatus, %0" :: "r"((uint32_t)v)); }
static inline uint64_t arch_read_sie(void) { uint32_t v; __asm__ __volatile__("csrr %0, sie" : "=r"(v)); return v; }
static inline void arch_write_sie(uint64_t v) { __asm__ __volatile__("csrw sie, %0" :: "r"((uint32_t)v)); }
static inline uint64_t arch_read_sip(void) { uint32_t v; __asm__ __volatile__("csrr %0, sip" : "=r"(v)); return v; }
static inline void arch_write_sip(uint64_t v) { __asm__ __volatile__("csrw sip, %0" :: "r"((uint32_t)v)); }
static inline uint64_t arch_read_sscratch(void) { uint32_t v; __asm__ __volatile__("csrr %0, sscratch" : "=r"(v)); return v; }
static inline void arch_write_sscratch(uint64_t v) { __asm__ __volatile__("csrw sscratch, %0" :: "r"((uint32_t)v)); }

static inline void __attribute__((noreturn)) arch_halt(void) {
    arch_local_irq_disable();
    while (1)
        __asm__ __volatile__("wfi");
}

static inline int arch_is_kernel_address(const void *ptr) {
    return (uintptr_t)ptr >= PHYS_MEMORY_BASE;
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

static inline int arch_unwind_frames(uint64_t fp, struct backtrace_frame *frames, int max_frames) {
    int n = 0;
    for (int i = 0; i < max_frames && fp; i++) {
        uint32_t *frame = (uint32_t *)(uintptr_t)fp;
        if (!arch_is_kernel_address(frame))
            break;
        uint32_t ra = frame[-1];
        uint32_t next_fp = frame[-2];
        if (!ra)
            break;
        frames[n++].pc = ra;
        if (!next_fp || next_fp <= fp)
            break;
        fp = next_fp;
    }
    return n;
}


/* Local-only flush: safe while holding a spinlock (IRQs off).  Callers must
 * publish the remote flush after dropping the lock. */
static inline void arch_tlb_flush_page_local(uint64_t addr) {
    arch_tlb_flush_page(addr);
}
/* Local-only full flush. */
static inline void arch_tlb_flush_local(void) {
    arch_tlb_flush();
}
#endif
