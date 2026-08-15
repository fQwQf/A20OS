#ifndef _ARCH_LOONGARCH32_CPU_H
#define _ARCH_LOONGARCH32_CPU_H

#define ARCH_HAS_LOCAL_TLB_FLUSH 1

#include "core/types.h"
#include "core/consts.h"
#include "platform.h"

static inline void arch_mb(void)  { __asm__ __volatile__("dbar 0" ::: "memory"); }
static inline void arch_rmb(void) { __asm__ __volatile__("dbar 0" ::: "memory"); }
static inline void arch_wmb(void) { __asm__ __volatile__("dbar 0" ::: "memory"); }
static inline void arch_wfi(void) { __asm__ __volatile__("idle 0"); }
#define ARCH_HAS_SAFE_IDLE_WAIT 1
void arch_idle_wait(void);
static inline void arch_cpu_relax(void) { __asm__ __volatile__("nop"); }
static inline void arch_fence_i(void) {
    __asm__ __volatile__(
        "dbar 0\n\t"
        "ibar 0\n\t"
        ::: "memory"
    );
}
static inline void arch_flush_icache_range(const void *addr, size_t size) { (void)addr; (void)size; arch_fence_i(); }
static inline unsigned arch_current_cpu_id(void) {
    uint32_t id;
    __asm__ __volatile__("csrrd %0, 0x20" : "=r"(id));
    return (unsigned)id;
}
static inline void arch_local_irq_disable(void) {
    uint32_t val;
    __asm__ __volatile__("csrrd %0, 0x0" : "=r"(val));
    val &= ~(1UL << 2);
    __asm__ __volatile__("csrwr %0, 0x0" :: "r"(val));
}
static inline void arch_local_irq_enable(void) {
    uint32_t val;
    __asm__ __volatile__("csrrd %0, 0x0" : "=r"(val));
    val |= (1UL << 2);
    __asm__ __volatile__("csrwr %0, 0x0" :: "r"(val));
}
static inline void arch_irqchip_init(void) {
    uint32_t ecfg;
    __asm__ __volatile__("csrrd %0, 0x4" : "=r"(ecfg));
    ecfg |= (1UL << 11) | (1UL << 2);
    __asm__ __volatile__("csrwr %0, 0x4" :: "r"(ecfg));
}
static inline void arch_irqchip_enable(void) {
    uint32_t ecfg;
    __asm__ __volatile__("csrrd %0, 0x4" : "=r"(ecfg));
    ecfg |= (1UL << 2);
    __asm__ __volatile__("csrwr %0, 0x4" :: "r"(ecfg));
}
static inline int arch_irqs_enabled(void) {
    uint32_t val;
    __asm__ __volatile__("csrrd %0, 0x0" : "=r"(val));
    return !!(val & (1UL << 2));
}

static inline uint32_t arch_irq_save(void) {
    uint32_t flags;
    __asm__ __volatile__("csrrd %0, 0x0" : "=r"(flags));
    arch_local_irq_disable();
    return flags;
}

static inline void arch_irq_restore(uint32_t flags) {
    __asm__ __volatile__("csrwr %0, 0x0" :: "r"(flags));
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

static inline void arch_tlb_flush_local(void) {
    __asm__ __volatile__("invtlb 0, $zero, $zero" ::: "memory");
}
static inline void arch_tlb_flush(void) {
    arch_tlb_flush_local();
}
static inline void arch_tlb_flush_page_local_impl(uint32_t addr) {
    (void)addr;
    __asm__ __volatile__("invtlb 0, $zero, $zero" ::: "memory");
}
static inline void arch_tlb_flush_page(uint32_t addr) {
    arch_tlb_flush_page_local_impl(addr);
}
static inline void arch_tlb_flush_page_local(uint32_t addr) {
    arch_tlb_flush_page_local_impl(addr);
}

/* Task pointer lives in SAVE1 (CSR 0x31); SAVE0 (0x30) is reserved for the
 * kernel sp during user↔kernel traps.  The scheduler also needs the task in
 * the $tp register: __switch stores the outgoing stack through $tp.  The
 * first context switch leaves the boot context, where $tp is otherwise 0 —
 * setting it here keeps the save valid instead of writing to address 0. */
static inline void arch_set_task_pointer(void *task) {
    __asm__ __volatile__("move $tp, %0" :: "r"(task));
    uintptr_t value = (uintptr_t)task;
    __asm__ __volatile__("csrwr %0, 0x31"
                         : "+r"(value)
                         :
                         : "memory");
}
static inline void *arch_get_task_pointer(void) {
    void *tp;
    __asm__ __volatile__("csrrd %0, 0x31" : "=r"(tp));
    return tp;
}

static inline uint32_t arch_read_ra(void) {
    uint32_t ra;
    __asm__ __volatile__("move %0, $ra" : "=r"(ra));
    return ra;
}

/* CSR 0x5 = ESTAT. Synthetic cause compatible with trap_handler:
 *   Interrupt: (1UL << 31) | irq_number
 *   Exception: Ecode from ESTAT[21:16]
 */
static inline uint32_t arch_read_cause(void) {
    uint32_t estat;
    __asm__ __volatile__("csrrd %0, 0x5" : "=r"(estat));
    uint32_t ecode = (estat >> 16) & 0x3F;
    if (ecode != 0) {
        return ecode;
    }
    uint32_t is = estat & 0xFFFF;
    if (is) {
        int irq = __builtin_ctzl(is);
        return (1UL << 31) | (uint32_t)irq;
    }
    return 0;
}
static inline uint32_t arch_read_epc(void) {
    uint32_t v;
    __asm__ __volatile__("csrrd %0, 0x6" : "=r"(v));
    return v;
}
/* CSR 0x7 = BADV (Bad Virtual Address) */
static inline uint32_t arch_read_tval(void) {
    uint32_t v;
    __asm__ __volatile__("csrrd %0, 0x7" : "=r"(v));
    return v;
}
static inline void arch_write_epc(uint32_t v) {
    __asm__ __volatile__("csrwr %0, 0x6" :: "r"(v));
}
/* CSR 0xC = EENTRY (Exception Entry) */
static inline void arch_write_tvec(uint32_t v) {
    __asm__ __volatile__("csrwr %0, 0xC" :: "r"(v));
}

/* Page table base: CSR 0x19 = PGDL (physical address of the root table) */
static inline uint32_t arch_read_satp(void) {
    uint32_t v;
    __asm__ __volatile__("csrrd %0, 0x19" : "=r"(v));
    return v;
}
static inline void arch_write_satp(uint32_t v) {
    __asm__ __volatile__("csrwr %0, 0x19" :: "r"(v));
}
static inline uint32_t arch_read_addr_space_token(void) { return arch_read_satp(); }
static inline void arch_write_addr_space_token(uint32_t v) { arch_write_satp(v); }

/* PRMD for interrupt status */
static inline uint32_t arch_read_sstatus(void) {
    uint32_t v;
    __asm__ __volatile__("csrrd %0, 0x1" : "=r"(v));
    return v;
}
static inline void arch_write_sstatus(uint32_t v) {
    __asm__ __volatile__("csrwr %0, 0x1" :: "r"(v));
}

/* No direct SIE equivalent; use CRMD IE bit */
static inline uint32_t arch_read_sie(void) {
    return arch_irqs_enabled() ? (uint32_t)-1 : 0;
}
static inline void arch_write_sie(uint32_t v) {
    if (v) arch_local_irq_enable(); else arch_local_irq_disable();
}

static inline uint32_t arch_read_sip(void) {
    uint32_t v;
    __asm__ __volatile__("csrrd %0, 0x5" : "=r"(v));
    return v;
}
static inline void arch_write_sip(uint32_t v) {
    (void)v;
}

static inline uint32_t arch_read_sscratch(void) { return 0; }
static inline void arch_write_sscratch(uint32_t v) { (void)v; }

static inline void arch_halt(void) {
    arch_local_irq_disable();
    while (1) __asm__ __volatile__("idle 0");
}

static inline void arch_dcache_flush(uintptr_t addr, size_t size) {
    uintptr_t end = addr + size;
    addr &= ~(64UL - 1);
    while (addr < end) {
        __asm__ __volatile__("cacop 0x11, %0, 0" :: "r"(addr) : "memory");
        addr += 64;
    }
    __asm__ __volatile__("dbar 0" ::: "memory");
}

static inline int arch_is_kernel_address(const void *ptr) {
    uintptr_t v = (uintptr_t)ptr;
    if (!v)
        return 0;
    for (size_t i = 0; i < arch_ram_range_count(); i++) {
        paddr_t base, end;
        if (arch_ram_range(i, &base, &end) == 0 &&
            v >= PAGE_OFFSET + base && v < PAGE_OFFSET + end)
            return 1;
    }
    return 0;
}

static inline void arch_dma_sync_for_device(const void *addr, size_t size) {
    arch_dcache_flush((uintptr_t)addr, size);
}

static inline void arch_dma_sync_for_cpu(const void *addr, size_t size) {
    arch_dcache_flush((uintptr_t)addr, size);
}

struct backtrace_frame {
    uint32_t pc;
};

static inline int arch_unwind_frames(uint32_t fp,
                                     struct backtrace_frame *frames,
                                     int max_frames) {
    int n = 0;
    for (int i = 0; i < max_frames && fp; i++) {
        uint32_t *frame = (uint32_t *)fp;
        if (!arch_is_kernel_address(frame))
            break;
        uint32_t ra = frame[-1];
        uint32_t next_fp = frame[-2];
        if (!ra) break;
        frames[n++].pc = ra;
        if (!next_fp || next_fp <= fp) break;
        fp = next_fp;
    }
    return n;
}

#endif
