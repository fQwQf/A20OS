#ifndef A20_RISCV_MCU_CPU_H
#define A20_RISCV_MCU_CPU_H
#include "core/types.h"
#include "platform.h"

/* M-mode mechanism only; device interrupt routing belongs to the board. */
static inline void arch_mb(void) { __asm__ volatile("fence iorw,iorw" ::: "memory"); }
static inline void arch_rmb(void) { arch_mb(); }
static inline void arch_wmb(void) { arch_mb(); }
static inline void arch_wfi(void) { __asm__ volatile("wfi" ::: "memory"); }
static inline void arch_cpu_relax(void) { __asm__ volatile("nop"); }
static inline void arch_fence_i(void) { __asm__ volatile("fence.i" ::: "memory"); }
static inline void arch_flush_icache_range(const void *p, size_t n) { (void)p; (void)n; arch_fence_i(); }
static inline unsigned arch_current_cpu_id(void) { return 0; }
static inline void arch_local_irq_disable(void) { __asm__ volatile("csrci mstatus, 8" ::: "memory"); }
static inline void arch_local_irq_enable(void) { __asm__ volatile("csrsi mstatus, 8" ::: "memory"); }
static inline uint32_t arch_irq_save(void) {
    uint32_t flags;
    __asm__ volatile("csrrci %0, mstatus, 8" : "=r"(flags) :: "memory");
    return flags;
}
static inline void arch_irq_restore(uint32_t flags) {
    /* Restore only MIE: unrelated mstatus fields belong to the trap path. */
    if (flags & 8U) arch_local_irq_enable(); else arch_local_irq_disable();
}
static inline uint64_t arch_read_sstatus(void) { uint32_t v; __asm__ volatile("csrr %0, mstatus" : "=r"(v)); return v; }
static inline void arch_write_sstatus(uint64_t v) { __asm__ volatile("csrw mstatus, %0" :: "r"((uint32_t)v) : "memory"); }
static inline int arch_irqs_enabled(void) { return !!(arch_read_sstatus() & 8U); }
static inline int arch_local_irq_enabled(void) { return arch_irqs_enabled(); }
static inline void arch_irq_disable(void) { arch_local_irq_disable(); }
static inline void arch_irq_enable(void) { arch_local_irq_enable(); }
static inline void arch_tlb_flush(void) { arch_mb(); }
static inline void arch_tlb_flush_page(uint64_t a) { (void)a; arch_mb(); }
static inline void arch_tlb_flush_local(void) { arch_mb(); }
static inline void arch_tlb_flush_page_local(uint64_t a) { arch_tlb_flush_page(a); }
static inline void arch_set_task_pointer(void *p) { __asm__ volatile("mv tp, %0" :: "r"(p) : "memory"); }
static inline void *arch_get_task_pointer(void) { void *p; __asm__ volatile("mv %0, tp" : "=r"(p)); return p; }
static inline uint64_t arch_read_ra(void) { return (uintptr_t)__builtin_return_address(0); }
static inline uint64_t arch_read_cycle(void) {
    uint32_t hi, lo, again;
    do {
        __asm__ volatile("rdcycleh %0" : "=r"(hi));
        __asm__ volatile("rdcycle %0" : "=r"(lo));
        __asm__ volatile("rdcycleh %0" : "=r"(again));
    } while (hi != again);
    return ((uint64_t)hi << 32) | lo;
}
#define MCU_CSR_READ(fn, csr) static inline uint64_t fn(void) { uint32_t v; __asm__ volatile("csrr %0, " #csr : "=r"(v)); return v; }
#define MCU_CSR_WRITE(fn, csr) static inline void fn(uint64_t v) { __asm__ volatile("csrw " #csr ", %0" :: "r"((uint32_t)v) : "memory"); }
MCU_CSR_READ(arch_read_cause, mcause)
MCU_CSR_READ(arch_read_epc, mepc)
MCU_CSR_READ(arch_read_tval, mtval)
MCU_CSR_READ(arch_read_sie, mie)
MCU_CSR_READ(arch_read_sip, mip)
MCU_CSR_READ(arch_read_sscratch, mscratch)
MCU_CSR_WRITE(arch_write_epc, mepc)
MCU_CSR_WRITE(arch_write_tvec, mtvec)
MCU_CSR_WRITE(arch_write_sie, mie)
MCU_CSR_WRITE(arch_write_sip, mip)
MCU_CSR_WRITE(arch_write_sscratch, mscratch)
#undef MCU_CSR_READ
#undef MCU_CSR_WRITE
static inline uint64_t arch_read_satp(void) { return 0; }
static inline void arch_write_satp(uint64_t v) { (void)v; }
static inline uint64_t arch_read_addr_space_token(void) { return 0; }
static inline void arch_write_addr_space_token(uint64_t v) { (void)v; }
static inline void __attribute__((noreturn)) arch_halt(void) { arch_local_irq_disable(); for (;;) arch_wfi(); }
static inline int arch_is_kernel_address(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return (a >= PHYS_MEMORY_BASE && a < PHYS_MEMORY_END) ||
           (a >= MCU_CODE_BASE && a < MCU_CODE_END);
}
static inline void arch_dma_sync_for_device(const void *p, size_t n) { (void)p; (void)n; arch_mb(); }
static inline void arch_dma_sync_for_cpu(const void *p, size_t n) { (void)p; (void)n; arch_mb(); }
struct backtrace_frame { uint64_t pc; };
static inline int arch_unwind_frames(uint64_t fp, struct backtrace_frame *f, int n) { (void)fp; (void)f; (void)n; return 0; }
#endif
