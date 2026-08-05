#ifndef _ARCH_RISCV64_CPU_H
#define _ARCH_RISCV64_CPU_H

#include "core/types.h"
#include "core/consts.h"
#include "platform.h"

static inline void arch_mb(void)  { __asm__ __volatile__("fence iorw,iorw" ::: "memory"); }
static inline void arch_rmb(void) { __asm__ __volatile__("fence ir,ir" ::: "memory"); }
static inline void arch_wmb(void) { __asm__ __volatile__("fence ow,ow" ::: "memory"); }
static inline void arch_wfi(void) { __asm__ __volatile__("wfi"); }
static inline void arch_cpu_relax(void) { __asm__ __volatile__("nop"); }
static inline void arch_fence_i(void) { __asm__ __volatile__("fence.i" ::: "memory"); }
static inline void arch_flush_icache_range(const void *addr, size_t size) { (void)addr; (void)size; arch_fence_i(); }
/* S-mode cannot read mhartid. Once process management is initialized, tp
 * points at the current task and supplies its logical CPU id. */
unsigned arch_current_cpu_id(void);
unsigned arch_cpu_hart_id(unsigned cpu_id);

static inline void arch_local_irq_disable(void) {
    __asm__ __volatile__("csrc sstatus, %0" :: "r"((uint64_t)(1UL << 1)));
}
static inline void arch_local_irq_enable(void) {
    __asm__ __volatile__("csrs sstatus, %0" :: "r"((uint64_t)(1UL << 1)));
}
static inline int arch_irqs_enabled(void) {
    uint64_t s;
    __asm__ __volatile__("csrr %0, sstatus" : "=r"(s));
    return !!(s & (1UL << 1));
}

void riscv64_remote_tlb_flush(uint64_t addr, uint64_t size);

static inline void arch_tlb_flush(void) {
    __asm__ __volatile__("sfence.vma" ::: "memory");
#ifdef CONFIG_SMP
    riscv64_remote_tlb_flush(0, ~(uint64_t)0);
#endif
}
static inline void arch_tlb_flush_page(uint64_t addr) {
    __asm__ __volatile__("sfence.vma %0, zero" :: "r"(addr) : "memory");
#ifdef CONFIG_SMP
    riscv64_remote_tlb_flush(addr, PAGE_SIZE);
#endif
}
/* Local-only page flush: safe while holding a spinlock (IRQs off).  The
 * caller must publish the remote flush after dropping the lock, or the
 * trap-return path must do it (see trap.c fault completion). */
static inline void arch_tlb_flush_page_local(uint64_t addr) {
    __asm__ __volatile__("sfence.vma %0, zero" :: "r"(addr) : "memory");
}
/* Local-only full flush. */
static inline void arch_tlb_flush_local(void) {
    __asm__ __volatile__("sfence.vma" ::: "memory");
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
    uint64_t ra;
    __asm__ __volatile__("mv %0, ra" : "=r"(ra));
    return ra;
}

static inline uint64_t arch_read_cause(void) {
    uint64_t v; __asm__ __volatile__("csrr %0, scause" : "=r"(v)); return v;
}
static inline uint64_t arch_read_epc(void) {
    uint64_t v; __asm__ __volatile__("csrr %0, sepc" : "=r"(v)); return v;
}
static inline uint64_t arch_read_tval(void) {
    uint64_t v; __asm__ __volatile__("csrr %0, stval" : "=r"(v)); return v;
}
static inline void arch_write_epc(uint64_t v) {
    __asm__ __volatile__("csrw sepc, %0" :: "r"(v));
}
static inline void arch_write_tvec(uint64_t v) {
    __asm__ __volatile__("csrw stvec, %0" :: "r"(v));
}
static inline uint64_t arch_read_satp(void) {
    uint64_t v; __asm__ __volatile__("csrr %0, satp" : "=r"(v)); return v;
}
static inline void arch_write_satp(uint64_t v) {
    __asm__ __volatile__("csrw satp, %0" :: "r"(v));
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
static inline uint64_t arch_read_sstatus(void) {
    uint64_t v; __asm__ __volatile__("csrr %0, sstatus" : "=r"(v)); return v;
}
static inline void arch_write_sstatus(uint64_t v) {
    __asm__ __volatile__("csrw sstatus, %0" :: "r"(v));
}
static inline uint64_t arch_read_sie(void) {
    uint64_t v; __asm__ __volatile__("csrr %0, sie" : "=r"(v)); return v;
}
static inline void arch_write_sie(uint64_t v) {
    __asm__ __volatile__("csrw sie, %0" :: "r"(v));
}
static inline uint64_t arch_read_sip(void) {
    uint64_t v; __asm__ __volatile__("csrr %0, sip" : "=r"(v)); return v;
}
static inline void arch_write_sip(uint64_t v) {
    __asm__ __volatile__("csrw sip, %0" :: "r"(v));
}
static inline uint64_t arch_read_sscratch(void) {
    uint64_t v; __asm__ __volatile__("csrr %0, sscratch" : "=r"(v)); return v;
}
static inline void arch_write_sscratch(uint64_t v) {
    __asm__ __volatile__("csrw sscratch, %0" :: "r"(v));
}

static inline void __attribute__((noreturn)) arch_halt(void) {
    __asm__ __volatile("csrw sie, zero");
    while (1) __asm__ __volatile("wfi");
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
        uint64_t ra = frame[-1];
        uint64_t next_fp = frame[-2];
        if (!ra) break;
        frames[n++].pc = ra;
        if (!next_fp || next_fp <= fp) break;
        fp = next_fp;
    }
    return n;
}

#endif
