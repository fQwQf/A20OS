#ifdef CONFIG_X86_64

#include "core/trap.h"
#include "core/cpu.h"
#include "proc/proc.h"
#include "core/timer.h"
#include "drivers/char/uart.h"
#include "drivers/core/driver_hwapi.h"
#include "core/progress.h"
#include "platform.h"
#include "core/string.h"

#include "trap_frame.h"

#define PS2_MOUSE_IRQ_VECTOR 0x2c

static uint64_t trap_cause[CONFIG_NR_CPUS];
static uint64_t trap_epc[CONFIG_NR_CPUS];
static uint64_t trap_tval[CONFIG_NR_CPUS];

/* Per-CPU scratch used by syscall_entry before it can switch to the kernel
 * stack.  Only valid with interrupts disabled during the syscall window. */
__attribute__((aligned(16))) uint64_t __x86_64_syscall_scratch[CONFIG_NR_CPUS][16];

void x86_64_set_trap_state(uint64_t cause, uint64_t epc, uint64_t tval) {
    unsigned cpu = cpu_current_id();
    trap_cause[cpu] = cause;
    trap_epc[cpu] = epc;
    trap_tval[cpu] = tval;
}

void x86_64_save_trap_state(uint64_t *cause, uint64_t *epc, uint64_t *tval) {
    unsigned cpu = cpu_current_id();
    *cause = trap_cause[cpu];
    *epc = trap_epc[cpu];
    *tval = trap_tval[cpu];
}

void x86_64_restore_trap_state(uint64_t cause, uint64_t epc, uint64_t tval) {
    x86_64_set_trap_state(cause, epc, tval);
}

uint64_t x86_64_get_trap_cause(void) { return trap_cause[cpu_current_id()]; }
uint64_t x86_64_get_trap_epc(void) { return trap_epc[cpu_current_id()]; }
uint64_t x86_64_get_trap_tval(void) { return trap_tval[cpu_current_id()]; }
void x86_64_set_trap_epc(uint64_t value) { trap_epc[cpu_current_id()] = value; }
static void handle_timer_irq(int from_user) {
    timer_irq_tick();
    kernel_progress_timer_tick();
    timer_set_interval(proc_next_timer_interval(timer_get_ticks()));
    proc_sched_tick(from_user);
}

static uint64_t gdt[CONFIG_NR_CPUS][8];

static struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdtr[CONFIG_NR_CPUS];

static struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) idt[256];

static struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idtr;

static struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss[CONFIG_NR_CPUS];

static uint8_t df_stack[CONFIG_NR_CPUS][4096 * 4] __attribute__((aligned(16)));

extern uint64_t isr_stub_table[256];
extern void syscall_entry(void);
extern void *syscall_entry_table[CONFIG_NR_CPUS];

#define MSR_EFER        0xC0000080
#define MSR_STAR        0xC0000081
#define MSR_LSTAR       0xC0000082
#define MSR_SFMASK      0xC0000084
#define EFER_SCE        (1ULL << 0)
#define EFER_NXE        (1ULL << 11)
#define RFLAGS_IF       (1ULL << 9)

static uint64_t make_gdt_entry(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    return ((uint64_t)(limit & 0xFFFF)) |
           ((uint64_t)(base & 0xFFFF) << 16) |
           ((uint64_t)((base >> 16) & 0xFF) << 32) |
           ((uint64_t)access << 40) |
           ((uint64_t)((limit >> 16) & 0xF) << 48) |
           ((uint64_t)(flags & 0xF) << 52) |
           ((uint64_t)((base >> 24) & 0xFF) << 56);
}

static void gdt_init(void) {
    unsigned cpu = cpu_current_id();
    uint64_t *cpu_gdt = gdt[cpu];
    cpu_gdt[0] = 0;
    cpu_gdt[1] = make_gdt_entry(0, 0xFFFFF, 0x9A, 0xA);
    cpu_gdt[2] = make_gdt_entry(0, 0xFFFFF, 0x92, 0xC);
    cpu_gdt[3] = make_gdt_entry(0, 0xFFFFF, 0xFA, 0xA);
    cpu_gdt[4] = make_gdt_entry(0, 0xFFFFF, 0xF2, 0xC);

    gdtr[cpu].limit = sizeof(gdt[cpu]) - 1;
    gdtr[cpu].base = (uint64_t)cpu_gdt;
    __asm__ __volatile__("lgdt %0" :: "m"(gdtr[cpu]));
    __asm__ __volatile__("mov $0x10, %%ax; mov %%ax, %%ds; mov %%ax, %%es; mov %%ax, %%fs; mov %%ax, %%gs; mov %%ax, %%ss" ::: "ax");
    __asm__ __volatile__(
        "pushq $0x08\n\t"
        "movabsq $1f, %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:" ::: "rax", "memory"
    );
}

static void tss_init(void) {
    unsigned cpu = cpu_current_id();
    memset(&tss[cpu], 0, sizeof(tss[cpu]));
    tss[cpu].ist1 = (uint64_t)df_stack[cpu] + sizeof(df_stack[cpu]);
    tss[cpu].iomap_base = sizeof(tss[cpu]);

    uint64_t tss_base = (uint64_t)&tss[cpu];
    gdt[cpu][5] = make_gdt_entry(tss_base & 0xFFFFFFFF, sizeof(tss[cpu]) - 1, 0x89, 0);
    /* High 32 bits of TSS base (for x86_64) */
    gdt[cpu][6] = (tss_base >> 32) & 0xFFFFFFFF;

    __asm__ __volatile__("ltr %%ax" :: "a"((uint16_t)0x28));
}

void x86_64_set_tss_rsp0(uint64_t rsp0) {
    tss[cpu_current_id()].rsp0 = rsp0;
}

static void idt_init(void) {
    if (cpu_current_id() == 0) {
        for (int i = 0; i < 256; i++) {
            uint64_t addr = isr_stub_table[i];
            idt[i].offset_low  = addr & 0xFFFF;
            idt[i].offset_mid  = (addr >> 16) & 0xFFFF;
            idt[i].offset_high = (addr >> 32) & 0xFFFFFFFF;
            idt[i].selector    = 0x08;
            /* Normal traps use task stacks; only double fault uses IST. */
            idt[i].ist         = (i == 8) ? 1 : 0;
            idt[i].type_attr   = 0x8E;
            idt[i].reserved    = 0;
        }
        idt[0x80].type_attr = 0xEE; /* DPL=3 syscall gate */
        idtr.limit = sizeof(idt) - 1;
        idtr.base  = (uint64_t)&idt;
    }
    __asm__ __volatile__("lidt %0" :: "m"(idtr));
}

void x86_64_poll_console_input(void) {
    uart_handle_irq();
}

static void pic_init(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    outb(0x21, (uint8_t)~((1u << KEYBOARD_IRQ) | (1u << 2) |
                           (1u << UART0_IRQ)));
    outb(0xA1, (uint8_t)~(1u << 4));
}

static void pic_eoi(uint64_t vector) {
    if (vector >= 0x28)
        outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

static void lapic_enable(void) {
    lapic_write(LAPIC_SVR, 0x1FF);
    lapic_write(LAPIC_LVT_LINT0, cpu_current_id() == 0 ? 0x700 : LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
}

static void ioapic_init(void) {
    /* Mask every redirection entry before any device line is routed; the
     * reset state of unused entries is not guaranteed and an unmasked
     * stale entry would deliver spurious vectors once LAPIC is enabled. */
    uint32_t ver = ioapic_read(0x01);
    uint32_t max_entries = ((ver >> 16) & 0xFF) + 1;
    for (uint32_t i = 0; i < max_entries; i++) {
        ioapic_write(0x10U + i * 2U + 1U, 0);
        ioapic_write(0x10U + i * 2U, 0x10000U);
    }
}

void x86_64_route_pci_irq(uint32_t gsi, uint8_t vector) {
    /* PCI INTx is level-triggered and active-low.  Route the GSI to the
     * vector with physical destination mode aimed at the BSP (APIC ID 0).
     * The entry starts MASKED: only request_irq()'s auto-enable (the board
     * irqchip enable hook) unmasks it, so a device whose handler failed to
     * register can never hold a shared level line asserted with nobody
     * clearing its interrupt source. */
    uint32_t low_reg = 0x10U + gsi * 2U;
    ioapic_write(low_reg + 1U, 0);
    ioapic_write(low_reg, (uint32_t)vector | (1U << 13) | (1U << 15) |
                          (1U << 16));
}

/* X86_64_PCI_INTX_MODEL:
 * - QEMU q35 routes root-bus PCI INTx to GSI 20-23 with the swizzle
 *   GSI = 20 + ((dev + pin - 1) & 3).  This was verified empirically by
 *   routing the whole GSI 16-23 window and observing which vectors fire:
 *   dev 2/3/4 pin A land on GSI 22/23/20 respectively (the i440fx-style
 *   base of 16 does NOT match q35 hardware behavior).
 * - Each routed GSI owns vector 0x40 + gsi so arch_handle_irq() can hand
 *   the vector straight to driver_irq_dispatch() as the IRQ line id.
 * - Only bus 0 is routed: devices behind a bridge need the bridge swizzle
 *   and ACPI _PRT, which this platform does not parse; those transports
 *   keep the polling fallback by receiving -1. */
#define X86_64_PCI_VECTOR_BASE 0x40
#define X86_64_PCI_GSI_BASE    20U

int arch_pci_intx_irq(int bus, int dev, int func, int pin) {
    (void)func;
    if (bus != 0 || pin < 1 || pin > 4)
        return -1;
    /* The swizzle below is q35-only, verified empirically against QEMU
     * (dev 2/3/4 pin A land on GSI 22/23/20).  i440fx routes PIRQ through
     * SeaBIOS-programmed legacy IRQs instead, which cannot be resolved
     * without ACPI _PRT and PIRQ link programming, so it keeps the
     * polling fallback.  Identify the machine by its host bridge. */
    static int host_bridge_ok = -1;
    if (host_bridge_ok < 0) {
        uint32_t id = readl((const volatile void *)
                            (PCI_ECAM_BASE + 0U));
        host_bridge_ok = (id == 0x29c08086U) ? 1 : 0;
    }
    if (!host_bridge_ok)
        return -1;
    uint32_t gsi = X86_64_PCI_GSI_BASE +
                   (((uint32_t)dev + (uint32_t)pin - 1U) & 3U);
    uint8_t vector = (uint8_t)(X86_64_PCI_VECTOR_BASE + gsi);
    x86_64_route_pci_irq(gsi, vector);
    return (int)vector;
}

void x86_64_pci_irq_set_masked(int vector, int masked) {
    uint32_t gsi = (uint32_t)(vector - X86_64_PCI_VECTOR_BASE);
    if (gsi < 16U || gsi > 23U)
        return;
    uint32_t low_reg = 0x10U + gsi * 2U;
    uint32_t low = ioapic_read(low_reg);
    if (masked)
        low |= (1U << 16);
    else
        low &= ~(1U << 16);
    ioapic_write(low_reg, low);
}

void trap_init(void) {
    gdt_init();
    idt_init();
    tss_init();

    /* Enable fast system calls via syscall/sysret. */
    uint64_t efer;
    __asm__ __volatile__("rdmsr" : "=a"(efer) : "c"((uint32_t)MSR_EFER) : "rdx");
    efer |= EFER_SCE | EFER_NXE;
    __asm__ __volatile__("wrmsr" :: "c"((uint32_t)MSR_EFER), "a"((uint32_t)efer), "d"((uint32_t)(efer >> 32)));

    /* STAR: bits 47:32 = syscall CS=0x08 (SS=0x10); bits 63:48 = sysret
     * user CS=0x1b (SS=0x23).  We still return through iretq, but the MSR
     * must be programmed correctly so the syscall instruction loads CS=0x08. */
    uint64_t star = ((uint64_t)0x1bULL << 48) | ((uint64_t)0x08ULL << 32);
    __asm__ __volatile__("wrmsr" :: "c"((uint32_t)MSR_STAR), "a"((uint32_t)star), "d"((uint32_t)(star >> 32)));

    uint64_t lstar = (uint64_t)(uintptr_t)syscall_entry_table[cpu_current_id()];
    __asm__ __volatile__("wrmsr" :: "c"((uint32_t)MSR_LSTAR), "a"((uint32_t)lstar), "d"((uint32_t)(lstar >> 32)));

    /* SFMASK: clear IF on syscall entry so the assembly entry can use the
     * kernel stack scratch area without interruption.  iretq restores the
     * original RFLAGS (including IF) on return. */
    uint64_t sfmask = RFLAGS_IF;
    __asm__ __volatile__("wrmsr" :: "c"((uint32_t)MSR_SFMASK), "a"((uint32_t)sfmask), "d"((uint32_t)(sfmask >> 32)));

    if (cpu_current_id() == 0) {
        pic_init();
        ioapic_init();
    }
    lapic_enable();
    if (cpu_current_id() == 0) {
        /* Keyboard/mouse vectors are owned by the PS/2 drvmod module:
         * arch_handle_irq() hands them to driver_irq_dispatch(), and the
         * module registers its ISRs through the framework (request_irq). */
    }
}

void arch_handle_irq(uint64_t irq, int from_user) {
    (void)from_user;
    if (irq == IRQ_VECTOR_RESCHEDULE) {
        lapic_write(LAPIC_EOI, 0);
        proc_sched_handle_reschedule_ipi();
        return;
    } else if (irq == IRQ_VECTOR_TIMER) {
        handle_timer_irq(from_user);
    } else if (irq == IRQ_VECTOR_UART || irq == IRQ_VECTOR_KEYBOARD ||
               irq == PS2_MOUSE_IRQ_VECTOR || irq == IRQ_VECTOR_PCI ||
               (irq >= X86_64_PCI_VECTOR_BASE && irq < IRQ_VECTOR_RESCHEDULE)) {
        /* PCI vectors double as driver IRQ line ids (see X86_64_PCI_INTX_MODEL). */
        driver_irq_dispatch((uint32_t)irq);
    }
    if (irq == IRQ_VECTOR_KEYBOARD || irq == IRQ_VECTOR_UART ||
        irq == PS2_MOUSE_IRQ_VECTOR)
        pic_eoi(irq);
    lapic_write(LAPIC_EOI, 0);
}

#endif
