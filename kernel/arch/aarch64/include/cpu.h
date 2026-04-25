#ifndef _ARCH_AARCH64_CPU_H
#define _ARCH_AARCH64_CPU_H

#include "core/types.h"
#include "platform.h"

#define AARCH64_TRAP_FLAG_IRQ  (1UL << 0)

extern volatile uint64_t aarch64_trap_flags;

uint64_t aarch64_gic_ack(void);
uint64_t aarch64_decode_sync_cause(uint64_t esr);

static inline void arch_mb(void)  { __asm__ __volatile__("dmb ish" ::: "memory"); }
static inline void arch_rmb(void) { __asm__ __volatile__("dmb ishld" ::: "memory"); }
static inline void arch_wmb(void) { __asm__ __volatile__("dmb ishst" ::: "memory"); }
static inline void arch_wfi(void) { __asm__ __volatile__("wfi"); }
static inline void arch_fence_i(void) {
    __asm__ __volatile__(
        "ic iallu\n\t"
        "dsb ish\n\t"
        "isb"
        ::: "memory");
}

static inline void arch_local_irq_disable(void) {
    __asm__ __volatile__("msr daifset, #2" ::: "memory");
}

static inline void arch_local_irq_enable(void) {
    __asm__ __volatile__("msr daifclr, #2" ::: "memory");
}

static inline int arch_irqs_enabled(void) {
    uint64_t daif;
    __asm__ __volatile__("mrs %0, daif" : "=r"(daif));
    return (daif & (1UL << 7)) == 0;
}

static inline void arch_tlb_flush(void) {
    __asm__ __volatile__(
        "dsb ishst\n\t"
        "tlbi vmalle1\n\t"
        "dsb ish\n\t"
        "isb"
        ::: "memory");
}

static inline void arch_tlb_flush_page(uint64_t addr) {
    (void)addr;
    arch_tlb_flush();
}

static inline void arch_set_task_pointer(void *task) {
    __asm__ __volatile__("msr tpidr_el1, %0" :: "r"(task));
}

static inline void *arch_get_task_pointer(void) {
    void *tp;
    __asm__ __volatile__("mrs %0, tpidr_el1" : "=r"(tp));
    return tp;
}

static inline uint64_t arch_read_ra(void) {
    uint64_t ra;
    __asm__ __volatile__("mov %0, x30" : "=r"(ra));
    return ra;
}

static inline uint64_t arch_read_esr(void) {
    uint64_t v;
    __asm__ __volatile__("mrs %0, esr_el1" : "=r"(v));
    return v;
}

static inline uint64_t arch_read_cause(void) {
    if (aarch64_trap_flags & AARCH64_TRAP_FLAG_IRQ)
        return CAUSE_INTR_MASK | aarch64_gic_ack();
    return aarch64_decode_sync_cause(arch_read_esr());
}

static inline uint64_t arch_read_epc(void) {
    uint64_t v;
    __asm__ __volatile__("mrs %0, elr_el1" : "=r"(v));
    return v;
}

static inline uint64_t arch_read_tval(void) {
    uint64_t v;
    __asm__ __volatile__("mrs %0, far_el1" : "=r"(v));
    return v;
}

static inline void arch_write_epc(uint64_t v) {
    __asm__ __volatile__("msr elr_el1, %0" :: "r"(v));
}

static inline void arch_write_tvec(uint64_t v) {
    __asm__ __volatile__(
        "msr vbar_el1, %0\n\t"
        "isb"
        :: "r"(v)
        : "memory");
}

static inline uint64_t arch_read_satp(void) {
    uint64_t v;
    __asm__ __volatile__("mrs %0, ttbr0_el1" : "=r"(v));
    return v;
}

static inline void arch_write_satp(uint64_t v) {
    __asm__ __volatile__(
        "msr ttbr0_el1, %0\n\t"
        "isb"
        :: "r"(v)
        : "memory");
}

static inline uint64_t arch_read_sstatus(void) {
    uint64_t v;
    __asm__ __volatile__("mrs %0, daif" : "=r"(v));
    return v;
}

static inline void arch_write_sstatus(uint64_t v) {
    __asm__ __volatile__("msr daif, %0" :: "r"(v));
}

static inline uint64_t arch_read_sie(void) {
    return arch_irqs_enabled() ? 1 : 0;
}

static inline void arch_write_sie(uint64_t v) {
    if (v)
        arch_local_irq_enable();
    else
        arch_local_irq_disable();
}

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
    return (uintptr_t)ptr >= PAGE_OFFSET;
}

static inline void arch_dma_sync_for_device(const void *addr, size_t size) {
    (void)addr;
    (void)size;
    __asm__ __volatile__("dsb ishst" ::: "memory");
}

static inline void arch_dma_sync_for_cpu(const void *addr, size_t size) {
    (void)addr;
    (void)size;
    __asm__ __volatile__("dsb ish" ::: "memory");
}

#endif
