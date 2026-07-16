#ifdef CONFIG_X86_64

#include "core/trap.h"
#include "core/cpu.h"
#include "proc/proc.h"
#include "core/timer.h"
#include "drivers/char/uart.h"
#include "drivers/input/ps2.h"
#include "drivers/core/driver_hwapi.h"
#include "core/progress.h"
#include "platform.h"
#include "core/string.h"

#include "trap_frame.h"

#define PS2_MOUSE_IRQ_VECTOR 0x2c

uint64_t __x86_64_trap_cause;
uint64_t __x86_64_trap_epc;
uint64_t __x86_64_trap_tval;

/* Per-CPU scratch used by syscall_entry before it can switch to the kernel
 * stack.  Only valid with interrupts disabled during the syscall window. */
__attribute__((aligned(16))) uint64_t __x86_64_syscall_scratch[16];
static void handle_timer_irq(int from_user) {
    timer_irq_tick();
    kernel_progress_timer_tick();
    timer_set_interval(proc_next_timer_interval(timer_get_ticks()));
    if (!from_user) return;
    task_t *cur = proc_current();
    if (cur) {
        cur->total_time++;
        uint64_t now = timer_get_ticks();
        if (now - cur->exec_start >= TICKS_PER_SEC / 100)
            proc_yield();
    }
}

static uint64_t gdt[8];

static struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdtr;

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
} __attribute__((packed)) tss;

static uint8_t df_stack[4096 * 4] __attribute__((aligned(16)));

extern uint64_t isr_stub_table[256];
extern void syscall_entry(void);

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
    gdt[0] = 0;
    gdt[1] = make_gdt_entry(0, 0xFFFFF, 0x9A, 0xA);
    gdt[2] = make_gdt_entry(0, 0xFFFFF, 0x92, 0xC);
    gdt[3] = make_gdt_entry(0, 0xFFFFF, 0xFA, 0xA);
    gdt[4] = make_gdt_entry(0, 0xFFFFF, 0xF2, 0xC);

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base = (uint64_t)&gdt;
    __asm__ __volatile__("lgdt %0" :: "m"(gdtr));
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
    memset(&tss, 0, sizeof(tss));
    tss.ist1 = (uint64_t)df_stack + sizeof(df_stack);
    tss.iomap_base = sizeof(tss);

    uint64_t tss_base = (uint64_t)&tss;
    gdt[5] = make_gdt_entry(tss_base & 0xFFFFFFFF, sizeof(tss) - 1, 0x89, 0);
    /* High 32 bits of TSS base (for x86_64) */
    gdt[6] = (tss_base >> 32) & 0xFFFFFFFF;

    __asm__ __volatile__("ltr %%ax" :: "a"((uint16_t)0x28));
}

void x86_64_set_tss_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}

static void idt_init(void) {
    for (int i = 0; i < 256; i++) {
        uint64_t addr = isr_stub_table[i];
        idt[i].offset_low  = addr & 0xFFFF;
        idt[i].offset_mid  = (addr >> 16) & 0xFFFF;
        idt[i].offset_high = (addr >> 32) & 0xFFFFFFFF;
        idt[i].selector    = 0x08;
        /*
         * Normal traps must use the current task's kernel stack.  A shared
         * IST stack cannot hold a schedulable context: another task's trap
         * would overwrite the suspended call stack before it is resumed.
         */
        idt[i].ist         = (i == 8) ? 1 : 0;
        idt[i].type_attr   = 0x8E;
        idt[i].reserved    = 0;
    }
    idt[0x80].type_attr = 0xEE; /* DPL=3 syscall gate */

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;
    __asm__ __volatile__("lidt %0" :: "m"(idtr));
}

void x86_64_poll_console_input(void) {
    uart_handle_irq();
    ps2_input_handle_irq();
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
    lapic_write(LAPIC_LVT_LINT0, 0x700); /* ExtINT from the 8259 PIC */
    lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
}

void x86_64_route_pci_irq(uint32_t gsi, uint8_t vector) {
    /* Q35 exposes virtio-pci through INTx.  Route its GSI to the vector used
     * by the transport before the driver makes the queue available. */
    uint32_t low_reg = 0x10U + gsi * 2U;
    ioapic_write(low_reg + 1U, 0);
    ioapic_write(low_reg, vector);
}

static int keyboard_irq_wrapper(int irq, void *priv) {
    (void)irq;
    (void)priv;
    ps2_input_handle_irq();
    return 0;
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

    uint64_t lstar = (uint64_t)(uintptr_t)syscall_entry;
    __asm__ __volatile__("wrmsr" :: "c"((uint32_t)MSR_LSTAR), "a"((uint32_t)lstar), "d"((uint32_t)(lstar >> 32)));

    /* SFMASK: clear IF on syscall entry so the assembly entry can use the
     * kernel stack scratch area without interruption.  iretq restores the
     * original RFLAGS (including IF) on return. */
    uint64_t sfmask = RFLAGS_IF;
    __asm__ __volatile__("wrmsr" :: "c"((uint32_t)MSR_SFMASK), "a"((uint32_t)sfmask), "d"((uint32_t)(sfmask >> 32)));

    pic_init();
    lapic_enable();
    request_irq(KEYBOARD_IRQ, keyboard_irq_wrapper, 0, NULL);
    request_irq(PS2_MOUSE_IRQ_VECTOR, keyboard_irq_wrapper, 0, NULL);
}

void arch_handle_irq(uint64_t irq, int from_user) {
    (void)from_user;
    if (irq == IRQ_VECTOR_TIMER) {
        handle_timer_irq(from_user);
    } else if (irq == IRQ_VECTOR_UART || irq == IRQ_VECTOR_KEYBOARD ||
               irq == PS2_MOUSE_IRQ_VECTOR || irq == IRQ_VECTOR_PCI) {
        driver_irq_dispatch((uint32_t)irq);
    }
    if (irq == IRQ_VECTOR_KEYBOARD || irq == IRQ_VECTOR_UART ||
        irq == PS2_MOUSE_IRQ_VECTOR)
        pic_eoi(irq);
    lapic_write(LAPIC_EOI, 0);
}

#endif
