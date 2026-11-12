#ifndef _ARCH_X86_64_CPU_H
#define _ARCH_X86_64_CPU_H

#include "core/types.h"
#include "platform.h"

/* HLT-based idle is safe on x86_64: the LAPIC timer and cross-CPU reschedule
 * IPIs are delivered while halted, and STI+HLT cannot lose a wakeup (the
 * instruction after STI executes before interrupts are recognized, so a
 * pending interrupt is taken immediately after HLT parks the CPU). */
#define ARCH_HAS_SAFE_IDLE_WAIT 1

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
static inline void arch_idle_wait(void) {
    __asm__ __volatile__("sti; hlt" ::: "memory");
}
#ifdef CONFIG_SHARED_DEVICE_CALL
void shared_device_poll(void);
#endif
static inline void arch_cpu_relax(void) {
#ifdef CONFIG_SHARED_DEVICE_CALL
    shared_device_poll();
#endif
    __asm__ __volatile__("pause" ::: "memory");
}

/* An unused data descriptor records the logical CPU in its byte-granularity
 * limit. LSL reads the current CPU's GDT without touching task GS or causing a
 * CPUID VM exit. Boot/trampoline GDTs omit this slot, so early calls fall back. */
#define X86_CPU_ID_SELECTOR 0x38U
#define X86_CPU_ID_LIMIT_TAG 0xa2000U

static inline unsigned x86_cpu_id_from_gdt_limit(unsigned limit, unsigned valid) {
    unsigned id = limit ^ X86_CPU_ID_LIMIT_TAG;
    return valid && id < CONFIG_NR_CPUS ? id : CONFIG_NR_CPUS;
}

static inline unsigned arch_current_cpu_id(void) {
#if CONFIG_NR_CPUS == 1
    /* Single-CPU builds need no descriptor lookup or boot fallback.
     * Per-CPU state reduces to index 0, matching cpu_current_id()'s clamp. */
    return 0;
#else
    unsigned limit;
    unsigned char valid;
    __asm__ __volatile__("lsl %w2, %0; setz %1"
                         : "=r"(limit), "=qm"(valid)
                         : "r"(X86_CPU_ID_SELECTOR) : "cc");
    unsigned id = x86_cpu_id_from_gdt_limit(limit, valid);
    if (id < CONFIG_NR_CPUS)
        return id;
    /* Use CPUID leaf 1 initial APIC ID.  Reading x2APIC MSR 0x802 only works
     * when the CPU is in x2APIC mode, which the kernel does not enable. */
    uint32_t eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(1)
                         :);
    return x86_64_apic_to_cpu((unsigned)(ebx >> 24));
#endif
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

/* Enable SMEP (CR4 bit 20) and SMAP (CR4 bit 21) when the CPU advertises
 * them (CPUID leaf 7 EBX bits 7/20).  All kernel access to user memory goes
 * through the direct map after explicit PTE permission checks
 * (kernel/mm/mm.c), so no stac/clac windows are required: any supervisor
 * access to a user page indicates a bug and should fault loudly.  QEMU's
 * default qemu64 model does not advertise these features, so this is a
 * no-op there; real CPUs and -cpu host/max get the protection. */
static inline void x86_64_enable_smep_smap(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(0) :);
    if (eax < 7)
        return;
    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(7), "c"(0) :);
    uint64_t bits = 0;
    if (ebx & (1U << 7))   /* SMEP */
        bits |= 1UL << 20;
    if (ebx & (1U << 20))  /* SMAP */
        bits |= 1UL << 21;
    if (!bits)
        return;
    uint64_t cr4;
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= bits;
    __asm__ __volatile__("mov %0, %%cr4" :: "r"(cr4) : "memory");
}

static inline void arch_tlb_flush(void) {
    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
}
static inline void arch_tlb_flush_page(uint64_t addr) {
    __asm__ __volatile__("invlpg (%0)" :: "r"(addr) : "memory");
}
static inline void arch_tlb_flush_page_local(uint64_t addr) {
    arch_tlb_flush_page(addr);
}

/*
 * Per-CPU area addressed by the kernel GS base.
 *
 * In user mode MSR_GS_BASE holds the user's GS base (arch_prctl keeps it in
 * the lower canonical range) and MSR_KERNEL_GS_BASE holds this area's address;
 * SWAPGS on kernel entry makes %gs-relative addressing reach it, and the
 * MSR pair swaps back on return.  In kernel mode neither MSR is rewritten on
 * the syscall path: the task pointer moves through area.task (a plain store)
 * instead of a WRMSR, and the user's FS base stays in MSR_FS_BASE untouched
 * for the whole syscall because kernel code never reads FS.
 *
 * Boot keeps both MSRs pointing at the area (a no-op SWAPGS) until the first
 * user return publishes the task's real user GS base through MSR_KERNEL_GS_BASE.
 * Offsets are ABI: the syscall entry and __switch assembly hardcode them.
 */
struct x86_64_cpu_area {
    void *task;             /* 0:  current task (proc_current fast path)      */
    uint64_t kstack_top;    /* 8:  incoming task's kernel stack top (entry rsp) */
    uint64_t scratch_rsp;   /* 16: user rsp parked across the stack switch    */
    uint64_t scratch_rflags;/* 24: user rflags parked across the stack switch */
    uint64_t trap_cause;    /* 32: published trap cause (syscall: 0x80)       */
    uint64_t trap_epc;      /* 40: published faulting PC                      */
    uint64_t trap_tval;     /* 48: published fault address / 0                */
    uint64_t cpu_id;        /* 56: dense logical id, early/debug readers      */
};
#define X86_64_CPU_AREA_TASK_OFFSET    0
#define X86_64_CPU_AREA_KSTACK_OFFSET  8
#define X86_64_CPU_AREA_SP_OFFSET      16
#define X86_64_CPU_AREA_RFLAGS_OFFSET  24
#define X86_64_CPU_AREA_CAUSE_OFFSET   32
#define X86_64_CPU_AREA_EPC_OFFSET     40
#define X86_64_CPU_AREA_TVAL_OFFSET    48
#define X86_64_CPU_AREA_CPUID_OFFSET   56

/* Driver-module builds compile this header without the kernel's -DCONFIG_NR_CPUS
 * (they only need the inline helpers); the array itself lives in trap/irqchip.c
 * where the kernel build always defines it. */
#ifndef CONFIG_NR_CPUS
#define CONFIG_NR_CPUS 1
#endif

extern struct x86_64_cpu_area x86_64_cpu_areas[CONFIG_NR_CPUS];

static inline void *arch_get_task_pointer(void) {
    void *task;
    __asm__ __volatile__("movq %%gs:%c1, %0"
                         : "=r"(task)
                         : "i"(X86_64_CPU_AREA_TASK_OFFSET));
    return task;
}

static inline void arch_set_task_pointer(void *task) {
    __asm__ __volatile__("movq %0, %%gs:%c1"
                         :
                         : "r"(task), "i"(X86_64_CPU_AREA_TASK_OFFSET)
                         : "memory");
}

/* Point both GS MSRs at this CPU's area.  Used at per-CPU bringup, before any
 * user task exists: with both halves equal, SWAPGS is a no-op until the first
 * __return_to_user parks the task's user GS base in MSR_KERNEL_GS_BASE. */
static inline void x86_64_cpu_gs_init(unsigned cpu) {
    uint64_t base = (uint64_t)(uintptr_t)&x86_64_cpu_areas[cpu];
    uint32_t low = (uint32_t)base;
    uint32_t high = (uint32_t)(base >> 32);
    x86_64_cpu_areas[cpu].cpu_id = cpu;
    x86_64_cpu_areas[cpu].task = NULL;
    __asm__ __volatile__("wrmsr" :: "c"((uint32_t)0xC0000101),
                         "a"(low), "d"(high));
    __asm__ __volatile__("wrmsr" :: "c"((uint32_t)0xC0000102),
                         "a"(low), "d"(high));
}

/* Per-CPU trap-state accessors: the syscall entry publishes cause/epc/tval
 * with three GS-relative stores, and the generic trap machinery reaches the
 * same fields through these helpers.  GS is always the per-CPU area in kernel
 * mode, so no id lookup or array indexing is needed. */
static inline void x86_64_area_store(unsigned offset, uint64_t value) {
    __asm__ __volatile__("movq %0, %%gs:%c1"
                         :
                         : "r"(value), "i"(offset)
                         : "memory");
}
static inline uint64_t x86_64_area_load(unsigned offset) {
    uint64_t value;
    __asm__ __volatile__("movq %%gs:%c1, %0"
                         : "=r"(value)
                         : "i"(offset));
    return value;
}

/* User FS base (musl TLS) lives in MSR_FS_BASE for the whole kernel residency:
 * kernel code never rewrites it, so reads are exact and writes take effect
 * immediately.  The trap frame keeps a copy for cross-task state; these
 * helpers are the only in-kernel MSR access besides bringup. */
static inline uint64_t arch_read_user_fs_base(void) {
    uint32_t low, high;
    __asm__ __volatile__("rdmsr" : "=a"(low), "=d"(high) : "c"((uint32_t)0xC0000100));
    return (uint64_t)high << 32 | low;
}
static inline void arch_write_user_fs_base(uint64_t base) {
    __asm__ __volatile__("wrmsr" :: "c"((uint32_t)0xC0000100),
                         "a"((uint32_t)base), "d"((uint32_t)(base >> 32)));
}
/* In kernel mode the user GS base is parked in MSR_KERNEL_GS_BASE (the SWAPGS
 * inactive half); the active MSR_GS_BASE stays on the per-CPU area. */
static inline uint64_t arch_read_user_gs_base(void) {
    uint32_t low, high;
    __asm__ __volatile__("rdmsr" : "=a"(low), "=d"(high) : "c"((uint32_t)0xC0000102));
    return (uint64_t)high << 32 | low;
}
static inline void arch_write_user_gs_base(uint64_t base) {
    __asm__ __volatile__("wrmsr" :: "c"((uint32_t)0xC0000102),
                         "a"((uint32_t)base), "d"((uint32_t)(base >> 32)));
}

static inline uint64_t arch_read_ra(void) {
    uint64_t ra;
    __asm__ __volatile__("mov 0(%%rsp), %0" : "=r"(ra));
    return ra;
}

/* x86_64 does not have CSRs.  During trap entry we stash values into
 * these globals so that the generic C trap handler can read them. */
static inline uint64_t arch_read_cause(void)  { return x86_64_get_trap_cause(); }
static inline uint64_t arch_read_epc(void)    { return x86_64_get_trap_epc(); }
static inline uint64_t arch_read_tval(void)   { return x86_64_get_trap_tval(); }
static inline void arch_write_epc(uint64_t v) { x86_64_set_trap_epc(v); }
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
static inline void arch_flush_icache_range(const void *addr, size_t size) { (void)addr; (void)size; }

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

/* I/O APIC register window.  Two registers only: writing the index selects what
 * the read at +0x10 returns.  @base is a kernel-virtual address, so a controller
 * outside the direct map would need a mapping this inline does not add -- every
 * MADT entry this kernel accepts is below 4 GiB. */
static inline uint32_t ioapic_read_at(uintptr_t base, uint32_t reg) {
    *(volatile uint32_t *)(base + 0x00) = reg;
    return *(volatile uint32_t *)(base + 0x10);
}
static inline void ioapic_write_at(uintptr_t base, uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(base + 0x00) = reg;
    *(volatile uint32_t *)(base + 0x10) = val;
}

/* Controller 0.  A caller holding a specific GSI must use the lookup instead:
 * a machine with more than one I/O APIC does not route GSI N through
 * controller 0. */
static inline uint32_t ioapic_read(uint32_t reg) {
    return ioapic_read_at(arch_ioapic_base(0), reg);
}
static inline void ioapic_write(uint32_t reg, uint32_t val) {
    ioapic_write_at(arch_ioapic_base(0), reg, val);
}


/* Local-only full flush. */
static inline void arch_tlb_flush_local(void) {
    arch_tlb_flush();
}
#endif
