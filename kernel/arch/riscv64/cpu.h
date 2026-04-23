#ifndef _ARCH_RISCV64_CPU_H
#define _ARCH_RISCV64_CPU_H

#include "types.h"

static inline void arch_mb(void)  { __asm__ __volatile__("fence iorw,iorw" ::: "memory"); }
static inline void arch_rmb(void) { __asm__ __volatile__("fence ir,ir" ::: "memory"); }
static inline void arch_wmb(void) { __asm__ __volatile__("fence ow,ow" ::: "memory"); }
static inline void arch_wfi(void) { __asm__ __volatile__("wfi"); }
static inline void arch_fence_i(void) { __asm__ __volatile__("fence.i" ::: "memory"); }

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

static inline void arch_tlb_flush(void) {
    __asm__ __volatile__("sfence.vma" ::: "memory");
}
static inline void arch_tlb_flush_page(uint64_t addr) {
    __asm__ __volatile__("sfence.vma %0, zero" :: "r"(addr) : "memory");
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

#endif
