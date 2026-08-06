#ifndef _ARCH_ARMV7M_CPU_H
#define _ARCH_ARMV7M_CPU_H

#include "core/types.h"
#include "platform.h"

extern volatile uint32_t armv7m_trap_cause;
extern volatile uint32_t armv7m_fault_addr;
extern volatile uint32_t armv7m_fault_pc;
extern void *armv7m_task_pointer;

static inline void arch_mb(void) { __asm__ __volatile__("dmb" ::: "memory"); }
static inline void arch_rmb(void) { __asm__ __volatile__("dmb" ::: "memory"); }
static inline void arch_wmb(void) { __asm__ __volatile__("dmb" ::: "memory"); }
static inline void arch_wfi(void) { __asm__ __volatile__("wfi"); }
static inline void arch_cpu_relax(void) { __asm__ __volatile__("yield"); }
static inline void arch_fence_i(void) { __asm__ __volatile__("dsb\n\tisb" ::: "memory"); }
static inline void arch_flush_icache_range(const void *addr, size_t size) {
    (void)addr;
    (void)size;
    arch_fence_i();
}

static inline unsigned arch_current_cpu_id(void) { return 0; }

static inline uint32_t arch_read_primask(void) {
    uint32_t value;
    __asm__ __volatile__("mrs %0, primask" : "=r"(value));
    return value;
}

static inline void arch_write_primask(uint32_t value) {
    __asm__ __volatile__("msr primask, %0" :: "r"(value) : "memory");
}

static inline void arch_local_irq_disable(void) {
    __asm__ __volatile__("cpsid i" ::: "memory");
}

static inline void arch_local_irq_enable(void) {
    __asm__ __volatile__("cpsie i" ::: "memory");
}

static inline int arch_irqs_enabled(void) {
    return (arch_read_primask() & 1U) == 0;
}

static inline uint32_t arch_irq_save(void) {
    uint32_t flags = arch_read_primask();
    arch_local_irq_disable();
    return flags;
}

static inline void arch_irq_restore(uint32_t flags) {
    arch_write_primask(flags);
}

static inline void arch_irq_disable(void) { arch_local_irq_disable(); }
static inline void arch_irq_enable(void) { arch_local_irq_enable(); }
static inline int arch_local_irq_enabled(void) { return arch_irqs_enabled(); }
static inline void arch_tlb_flush(void) { arch_mb(); }
static inline void arch_tlb_flush_page(uint64_t addr) { (void)addr; arch_mb(); }
static inline void arch_tlb_flush_page_local(uint64_t addr) { arch_tlb_flush_page(addr); }

static inline void arch_set_task_pointer(void *task) { armv7m_task_pointer = task; }
static inline void *arch_get_task_pointer(void) { return armv7m_task_pointer; }

static inline uint32_t arch_read_ra(void) {
    return (uint32_t)(uintptr_t)__builtin_return_address(0);
}

static inline uint64_t arch_read_cycle(void) {
    /*
     * CYCCNT is optional in Cortex-M debug implementations and QEMU's STM32
     * model does not expose the full DWT block.  SysTick is always present.
     */
    return (uint64_t)(0x00FFFFFFUL -
                      (*(volatile uint32_t *)0xE000E018UL & 0x00FFFFFFUL));
}

static inline uint64_t arch_read_cause(void) { return armv7m_trap_cause; }
static inline uint64_t arch_read_epc(void) { return armv7m_fault_pc; }
static inline uint64_t arch_read_tval(void) { return armv7m_fault_addr; }
static inline void arch_write_epc(uint64_t value) { armv7m_fault_pc = (uint32_t)value; }
static inline void arch_write_tvec(uint64_t value) {
    *(volatile uint32_t *)0xE000ED08UL = (uint32_t)value;
    arch_fence_i();
}

static inline uint64_t arch_read_addr_space_token(void) { return 0; }
static inline void arch_write_addr_space_token(uint64_t value) { (void)value; }
static inline uint64_t arch_read_satp(void) { return 0; }
static inline void arch_write_satp(uint64_t value) { (void)value; }
static inline uint64_t arch_read_sstatus(void) { return arch_read_primask(); }
static inline void arch_write_sstatus(uint64_t value) { arch_write_primask((uint32_t)value); }
static inline uint64_t arch_read_sie(void) { return arch_irqs_enabled(); }
static inline void arch_write_sie(uint64_t value) {
    if (value) arch_local_irq_enable(); else arch_local_irq_disable();
}
static inline uint64_t arch_read_sip(void) { return 0; }
static inline void arch_write_sip(uint64_t value) { (void)value; }
static inline uint64_t arch_read_sscratch(void) { return 0; }
static inline void arch_write_sscratch(uint64_t value) { (void)value; }

static inline void __attribute__((noreturn)) arch_halt(void) {
    arch_local_irq_disable();
    for (;;)
        arch_wfi();
}

static inline int arch_is_kernel_address(const void *ptr) {
    uintptr_t value = (uintptr_t)ptr;
    return value >= FLASH_MEMORY_BASE || value >= PHYS_MEMORY_BASE;
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

struct backtrace_frame { uint64_t pc; };
static inline int arch_unwind_frames(uint64_t fp, struct backtrace_frame *frames,
                                     int max_frames) {
    (void)fp;
    (void)frames;
    (void)max_frames;
    return 0;
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
