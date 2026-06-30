#ifndef _ARCH_X86_64_CPU_H
#define _ARCH_X86_64_CPU_H

#include "core/types.h"
#include "platform.h"

static inline void arch_mb(void) {
    __asm__ __volatile__("lock; addl $0, 0(%%rsp)" ::: "memory");
}
static inline void arch_rmb(void) {
    __asm__ __volatile__("lfence" ::: "memory");
}
static inline void arch_wmb(void) {
    __asm__ __volatile__("sfence" ::: "memory");
}
static inline void arch_wfi(void) {
    __asm__ __volatile__("sti; hlt");
}

static inline unsigned arch_current_cpu_id(void) {
    uint32_t eax = 1, ebx, ecx, edx;
    __asm__ __volatile__("cpuid"
                         : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    return (unsigned)(ebx >> 24);
}

static inline void arch_local_irq_disable(void) {
    __asm__ __volatile__("cli" ::: "memory");
}
static inline void arch_local_irq_enable(void) {
    __asm__ __volatile__("sti" ::: "memory");
}
static inline int arch_irqs_enabled(void) {
    uint64_t rflags;
    __asm__ __volatile__("pushfq; popq %0" : "=r"(rflags));
    return !!(rflags & (1UL << 9));
}

static inline void x86_64_enable_fpu_sse(void) {
    uint64_t cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1UL << 2);  /* EM: allow x87/SSE instructions. */
    cr0 &= ~(1UL << 3);  /* TS: do not fault on first FPU use. */
    cr0 |=  (1UL << 1);  /* MP: monitor WAIT/FWAIT with TS. */
    __asm__ __volatile__("mov %0, %%cr0" :: "r"(cr0) : "memory");

    uint64_t cr4;
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1UL << 9);   /* OSFXSR: enable FXSAVE/FXRSTOR and SSE. */
    cr4 |= (1UL << 10);  /* OSXMMEXCPT: enable unmasked SSE exceptions. */
    __asm__ __volatile__("mov %0, %%cr4" :: "r"(cr4) : "memory");

    __asm__ __volatile__("fninit");
}

static inline void arch_tlb_flush(void) {
    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
}
static inline void arch_tlb_flush_page(uint64_t addr) {
    __asm__ __volatile__("invlpg (%0)" :: "r"(addr) : "memory");
}


static inline void arch_set_task_pointer(void *task) {
    (void)task;
}
static inline void *arch_get_task_pointer(void) {
    return NULL;
}

static inline uint64_t arch_read_ra(void) {
    uint64_t ra;
    __asm__ __volatile__("mov 0(%%rsp), %0" : "=r"(ra));
    return ra;
}

/* x86_64 does not have CSRs.  During trap entry we stash values into
 * these globals so that the generic C trap handler can read them. */
extern uint64_t __x86_64_trap_cause;
extern uint64_t __x86_64_trap_epc;
extern uint64_t __x86_64_trap_tval;

static inline uint64_t arch_read_cause(void)  { return __x86_64_trap_cause; }
static inline uint64_t arch_read_epc(void)    { return __x86_64_trap_epc; }
static inline uint64_t arch_read_tval(void)   { return __x86_64_trap_tval; }
static inline void arch_write_epc(uint64_t v) { __x86_64_trap_epc = v; }
static inline void arch_write_tvec(uint64_t v) { (void)v; /* IDT fixed in entry.S */ }

static inline uint64_t arch_read_satp(void) {
    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}
static inline void arch_write_satp(uint64_t v) {
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(v) : "memory");
}
static inline uint64_t arch_read_addr_space_token(void) { return arch_read_satp(); }
static inline void arch_write_addr_space_token(uint64_t v) { arch_write_satp(v); }

static inline uint64_t arch_read_sstatus(void) {
    uint64_t rflags;
    __asm__ __volatile__("pushfq; popq %0" : "=r"(rflags));
    return rflags;
}
static inline void arch_write_sstatus(uint64_t v) {
    __asm__ __volatile__("push %0; popfq" :: "rm"(v) : "memory");
}

static inline void arch_fence_i(void) {
    __asm__ __volatile__("mfence" ::: "memory");
}

/* SIE / SIP have no direct x86 equivalent; we keep interrupts enabled
 * globally via IF and use the LAPIC mask bits per vector.  Stubs. */
static inline uint64_t arch_read_sie(void)  { return arch_irqs_enabled(); }
static inline void arch_write_sie(uint64_t v) { if (v) arch_local_irq_enable(); else arch_local_irq_disable(); }
static inline uint64_t arch_read_sip(void)  { return 0; }
static inline void arch_write_sip(uint64_t v) { (void)v; }
static inline uint64_t arch_read_sscratch(void) { return 0; }
static inline void arch_write_sscratch(uint64_t v) { (void)v; }

static inline void __attribute__((noreturn)) arch_halt(void) {
    arch_local_irq_disable();
    while (1) __asm__ __volatile__("hlt");
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

static inline int arch_unwind_frames(uint64_t rbp,
                                     struct backtrace_frame *frames,
                                     int max_frames) {
    int n = 0;
    for (int i = 0; i < max_frames && rbp; i++) {
        uint64_t *frame = (uint64_t *)rbp;
        if (!arch_is_kernel_address(frame))
            break;
        uint64_t ra = frame[1];
        uint64_t next_rbp = frame[0];
        if (!ra) break;
        frames[n++].pc = ra;
        if (!next_rbp || next_rbp <= rbp) break;
        rbp = next_rbp;
    }
    return n;
}

/* I/O port helpers */
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ __volatile__("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ __volatile__("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ __volatile__("outw %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ __volatile__("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ __volatile__("outl %0, %1" :: "a"(val), "Nd"(port));
}

/* MSR helpers */
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ __volatile__("wrmsr" :: "a"(lo), "d"(hi), "c"(msr));
}

/* LAPIC helpers */
static inline uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t *)(LAPIC_BASE + reg);
}
static inline void lapic_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(LAPIC_BASE + reg) = val;
}

/* IOAPIC helpers */
static inline uint32_t ioapic_read(uint32_t reg) {
    *(volatile uint32_t *)(IOAPIC_BASE + 0x00) = reg;
    return *(volatile uint32_t *)(IOAPIC_BASE + 0x10);
}
static inline void ioapic_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(IOAPIC_BASE + 0x00) = reg;
    *(volatile uint32_t *)(IOAPIC_BASE + 0x10) = val;
}

#endif
