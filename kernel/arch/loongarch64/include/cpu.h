#ifndef _ARCH_LOONGARCH64_CPU_H
#define _ARCH_LOONGARCH64_CPU_H

#include "core/types.h"
#include "platform.h"

static inline void arch_mb(void)  { __asm__ __volatile__("dbar 0" ::: "memory"); }
static inline void arch_rmb(void) { __asm__ __volatile__("dbar 0" ::: "memory"); }
static inline void arch_wmb(void) { __asm__ __volatile__("dbar 0" ::: "memory"); }
static inline void arch_wfi(void) { __asm__ __volatile__("idle 0"); }
static inline void arch_fence_i(void) { 
    __asm__ __volatile__(
        "dbar 0\n\t"
        "ibar 0\n\t"
        ::: "memory"
    ); 
}
static inline unsigned arch_current_cpu_id(void) {
    uint64_t id;
    __asm__ __volatile__("csrrd %0, 0x20" : "=r"(id));
    return (unsigned)id;
}
static inline void arch_local_irq_disable(void) {
    uint64_t val;
    __asm__ __volatile("csrrd %0, 0x0" : "=r"(val));
    val &= ~(1UL << 2);
    __asm__ __volatile("csrwr %0, 0x0" :: "r"(val));
}
static inline void arch_local_irq_enable(void) {
    uint64_t val;
    __asm__ __volatile("csrrd %0, 0x0" : "=r"(val));
    val |= (1UL << 2);
    __asm__ __volatile("csrwr %0, 0x0" :: "r"(val));
}
static inline int arch_irqs_enabled(void) {
    uint64_t val;
    __asm__ __volatile("csrrd %0, 0x0" : "=r"(val));
    return !!(val & (1UL << 2));
}

static inline void arch_tlb_flush(void) {
    __asm__ __volatile("invtlb 0, $zero, $zero" ::: "memory");
}
static inline void arch_tlb_flush_page(uint64_t addr) {
    (void)addr;
    __asm__ __volatile("invtlb 0, $zero, $zero" ::: "memory");
}

/* Task pointer lives in SAVE1 (CSR 0x31).
 * SAVE0 (0x30) is reserved for kernel sp during user↔kernel traps.
 * These are called during proc_init / context_switch to set up the
 * SAVE1 register so that __trap_from_user can recover tp. */
static inline void arch_set_task_pointer(void *task) {
    __asm__ __volatile("csrwr %0, 0x31" :: "r"(task));
}
static inline void *arch_get_task_pointer(void) {
    void *tp;
    __asm__ __volatile("csrrd %0, 0x31" : "=r"(tp));
    return tp;
}

static inline uint64_t arch_read_ra(void) {
    uint64_t ra;
    __asm__ __volatile("move %0, $ra" : "=r"(ra));
    return ra;
}

/* CSR 0x5 = ESTAT. Returns synthetic cause value compatible with trap_handler:
 *   Interrupt: (1UL << 63) | irq_number   (highest pending IS bit)
 *   Exception: Ecode from ESTAT[21:16]
 */
static inline uint64_t arch_read_cause(void) {
    uint64_t estat;
    __asm__ __volatile("csrrd %0, 0x5" : "=r"(estat));
    uint64_t ecode = (estat >> 16) & 0x3F;
    if (ecode != 0) {
        return ecode;
    }
    uint64_t is = estat & 0xFFFF;
    if (is) {
        int irq = __builtin_ctzl(is);
        return (1UL << 63) | (uint64_t)irq;
    }
    return 0;
}
static inline uint64_t arch_read_epc(void) {
    uint64_t v;
    __asm__ __volatile("csrrd %0, 0x6" : "=r"(v));
    return v;
}
/* CSR 0x7 = BADV (Bad Virtual Address) */
static inline uint64_t arch_read_tval(void) {
    uint64_t v;
    __asm__ __volatile("csrrd %0, 0x7" : "=r"(v));
    return v;
}
static inline void arch_write_epc(uint64_t v) {
    __asm__ __volatile("csrwr %0, 0x6" :: "r"(v));
}
/* CSR 0xC = EENTRY (Exception Entry) */
static inline void arch_write_tvec(uint64_t v) {
    __asm__ __volatile("csrwr %0, 0xC" :: "r"(v));
}

/* Page table base: CSR 0x19 = PGDL */
static inline uint64_t arch_read_satp(void) {
    uint64_t v;
    __asm__ __volatile("csrrd %0, 0x19" : "=r"(v));
    return v;
}
static inline void arch_write_satp(uint64_t v) {
    __asm__ __volatile("csrwr %0, 0x19" :: "r"(v));
}

/* PRMD for interrupt status */
static inline uint64_t arch_read_sstatus(void) {
    uint64_t v;
    __asm__ __volatile("csrrd %0, 0x1" : "=r"(v));
    return v;
}
static inline void arch_write_sstatus(uint64_t v) {
    __asm__ __volatile("csrwr %0, 0x1" :: "r"(v));
}

/* No direct SIE equivalent; use CRMD IE bit */
static inline uint64_t arch_read_sie(void) {
    return arch_irqs_enabled() ? (uint64_t)-1 : 0;
}
static inline void arch_write_sie(uint64_t v) {
    if (v) arch_local_irq_enable(); else arch_local_irq_disable();
}

static inline uint64_t arch_read_sip(void) {
    uint64_t v;
    __asm__ __volatile("csrrd %0, 0x5" : "=r"(v));
    return v;
}
static inline void arch_write_sip(uint64_t v) {
    (void)v;
}

static inline uint64_t arch_read_sscratch(void) { return 0; }
static inline void arch_write_sscratch(uint64_t v) { (void)v; }

static inline void arch_halt(void) {
    arch_local_irq_disable();
    while (1) __asm__ __volatile__("idle 0");
}

static inline void arch_dcache_flush(uintptr_t addr, size_t size) {
    uintptr_t end = addr + size;
    addr &= ~(64UL - 1); // 按照 64 字节对齐
    while (addr < end) {
        // op=0x11: 对 L1 D-Cache 执行 Hit Writeback Invalidate
        __asm__ __volatile__("cacop 0x11, %0, 0" :: "r"(addr) : "memory");
        addr += 64;
    }
    // 确保所有的 Cache 写回操作彻底完成
    __asm__ __volatile__("dbar 0" ::: "memory"); 
}

static inline int arch_is_kernel_address(const void *ptr) {
    uintptr_t v = (uintptr_t)ptr;
    return v >= PHYS_MEMORY_BASE && v < PHYS_MEMORY_END;
}

static inline void arch_dma_sync_for_device(const void *addr, size_t size) {
    arch_dcache_flush((uintptr_t)addr, size);
}

static inline void arch_dma_sync_for_cpu(const void *addr, size_t size) {
    arch_dcache_flush((uintptr_t)addr, size);
}

#endif
