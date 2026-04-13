#include "arch_ops.h"
#include "loongarch64_consts.h"

/* LoongArch CSR indices used by this backend. */
#define LA_CSR_CRMD    0x0
#define LA_CSR_ECFG    0x4
#define LA_CSR_ESTAT   0x5
#define LA_CSR_ERA     0x6
#define LA_CSR_BADV    0x7
#define LA_CSR_EENTRY  0xC
#define LA_CSR_SAVE0   0x30
#define LA_CSR_TCFG    0x41
#define LA_CSR_TVAL    0x42

/* CRMD bits */
#define LA_CRMD_IE     (1UL << 2)

/* ESTAT/ECFG interrupt bits (IS/LIE[12:0]) */
#define LA_INT_SWI0    (1UL << 0)
#define LA_INT_SWI1    (1UL << 1)
#define LA_INT_HWI0    (1UL << 2)
#define LA_INT_HWI1    (1UL << 3)
#define LA_INT_HWI_MASK  (0xFFUL << 2)
#define LA_INT_TIMER   (1UL << 11)
#define LA_INT_IPI     (1UL << 12)
#define LA_INT_ALL_MASK 0x1FFFUL

/* Exception code for SYSCALL on LoongArch. */
#define LA_ECODE_SYSCALL 0x0BUL

static inline uint64_t la_csr_read(uint32_t csr) {
    uint64_t v;
    switch (csr) {
    case LA_CSR_CRMD:   __asm__ volatile("csrrd %0, 0x0" : "=r"(v)); break;
    case LA_CSR_ECFG:   __asm__ volatile("csrrd %0, 0x4" : "=r"(v)); break;
    case LA_CSR_ESTAT:  __asm__ volatile("csrrd %0, 0x5" : "=r"(v)); break;
    case LA_CSR_ERA:    __asm__ volatile("csrrd %0, 0x6" : "=r"(v)); break;
    case LA_CSR_BADV:   __asm__ volatile("csrrd %0, 0x7" : "=r"(v)); break;
    case LA_CSR_EENTRY: __asm__ volatile("csrrd %0, 0xC" : "=r"(v)); break;
    case LA_CSR_SAVE0:  __asm__ volatile("csrrd %0, 0x30" : "=r"(v)); break;
    case LA_CSR_TCFG:   __asm__ volatile("csrrd %0, 0x41" : "=r"(v)); break;
    case LA_CSR_TVAL:   __asm__ volatile("csrrd %0, 0x42" : "=r"(v)); break;
    default:            v = 0; break;
    }
    return v;
}

static inline void la_csr_write(uint32_t csr, uint64_t v) {
    switch (csr) {
    case LA_CSR_CRMD:   __asm__ volatile("csrwr %0, 0x0" :: "r"(v)); break;
    case LA_CSR_ECFG:   __asm__ volatile("csrwr %0, 0x4" :: "r"(v)); break;
    case LA_CSR_ESTAT:  __asm__ volatile("csrwr %0, 0x5" :: "r"(v)); break;
    case LA_CSR_ERA:    __asm__ volatile("csrwr %0, 0x6" :: "r"(v)); break;
    case LA_CSR_EENTRY: __asm__ volatile("csrwr %0, 0xC" :: "r"(v)); break;
    case LA_CSR_SAVE0:  __asm__ volatile("csrwr %0, 0x30" :: "r"(v)); break;
    case LA_CSR_TCFG:   __asm__ volatile("csrwr %0, 0x41" :: "r"(v)); break;
    default: break;
    }
}

typedef struct {
    uintptr_t base;
    uint8_t shift;
} la_uart_port_t;

#define UART_RHR 0
#define UART_THR 0
#define UART_IER 1
#define UART_FCR 2
#define UART_LCR 3
#define UART_MCR 4
#define UART_LSR 5
#define UART_IER_RX_ENABLE (1 << 0)
#define UART_LSR_RX_READY  (1 << 0)
#define UART_LSR_TX_IDLE   (1 << 5)

static const la_uart_port_t g_uart_ports[] = {
    { 0x1FE001E0UL, 0 },
    { 0x1FE001E0UL, 2 },
    { 0x1FE002E0UL, 0 },
    { 0x1FE002E0UL, 2 },
    { UART0_BASE, 0 },
};

static la_uart_port_t g_uart = {0, 0};
static int g_uart_ready = 0;

static inline uintptr_t reg_addr(uintptr_t base, uint8_t shift, uint32_t reg) {
    return base + ((uintptr_t)reg << shift);
}

static inline uint8_t reg_read(uintptr_t base, uint8_t shift, uint32_t reg) {
    return *(volatile uint8_t *)reg_addr(base, shift, reg);
}

static inline void reg_write(uintptr_t base, uint8_t shift, uint32_t reg, uint8_t val) {
    *(volatile uint8_t *)reg_addr(base, shift, reg) = val;
}

static int uart_try_port(const la_uart_port_t *p) {
    reg_write(p->base, p->shift, UART_IER, 0x00);
    reg_write(p->base, p->shift, UART_LCR, 0x80);
    reg_write(p->base, p->shift, 0, 0x03);
    reg_write(p->base, p->shift, 1, 0x00);
    reg_write(p->base, p->shift, UART_LCR, 0x03);
    reg_write(p->base, p->shift, UART_FCR, 0x07);
    reg_write(p->base, p->shift, UART_MCR, 0x0B);
    reg_write(p->base, p->shift, UART_IER, UART_IER_RX_ENABLE);

    for (uint32_t i = 0; i < 200000; i++) {
        if (reg_read(p->base, p->shift, UART_LSR) & UART_LSR_TX_IDLE) {
            return 0;
        }
        arch_cpu_relax();
    }
    return -1;
}

uint64_t arch_irq_save(void) {
    uint64_t crmd = la_csr_read(LA_CSR_CRMD);
    la_csr_write(LA_CSR_CRMD, crmd & ~LA_CRMD_IE);
    return crmd;
}

void arch_irq_restore(uint64_t state) {
    la_csr_write(LA_CSR_CRMD, state);
}

void arch_irq_enable(void) {
    uint64_t crmd = la_csr_read(LA_CSR_CRMD);
    la_csr_write(LA_CSR_CRMD, crmd | LA_CRMD_IE);
}

void arch_irq_disable(void) {
    uint64_t crmd = la_csr_read(LA_CSR_CRMD);
    la_csr_write(LA_CSR_CRMD, crmd & ~LA_CRMD_IE);
}

void arch_irq_enable_timer(void) {
    uint64_t ecfg = la_csr_read(LA_CSR_ECFG);
    la_csr_write(LA_CSR_ECFG, ecfg | LA_INT_TIMER);
}

void arch_irq_disable_timer(void) {
    uint64_t ecfg = la_csr_read(LA_CSR_ECFG);
    la_csr_write(LA_CSR_ECFG, ecfg & ~LA_INT_TIMER);
}

void arch_irq_enable_external(void) {
    uint64_t ecfg = la_csr_read(LA_CSR_ECFG);
    la_csr_write(LA_CSR_ECFG, ecfg | LA_INT_HWI0 | LA_INT_HWI1);
}

void arch_softirq_clear(void) {
    uint64_t estat = la_csr_read(LA_CSR_ESTAT);
    estat &= ~(LA_INT_SWI0 | LA_INT_SWI1);
    la_csr_write(LA_CSR_ESTAT, estat);
}

void arch_trap_vector_set(uint64_t addr) {
    la_csr_write(LA_CSR_EENTRY, addr);
}

void arch_trap_scratch_set(uint64_t val) {
    la_csr_write(LA_CSR_SAVE0, val);
}

uint64_t arch_trap_cause(void) {
    uint64_t estat = la_csr_read(LA_CSR_ESTAT);
    uint64_t is = estat & LA_INT_ALL_MASK;

    if (is & LA_INT_TIMER) return ARCH_TRAP_INTERRUPT_MASK | ARCH_IRQ_CAUSE_TIMER;
    if (is & (LA_INT_SWI0 | LA_INT_SWI1)) return ARCH_TRAP_INTERRUPT_MASK | ARCH_IRQ_CAUSE_SOFT;
    if (is & (LA_INT_HWI_MASK | LA_INT_IPI)) return ARCH_TRAP_INTERRUPT_MASK | ARCH_IRQ_CAUSE_EXTERNAL;

    /* LoongArch ecode in ESTAT[21:16]. Normalize syscall to common value. */
    uint64_t ecode = (estat >> 16) & 0x3FUL;
    if (ecode == LA_ECODE_SYSCALL) return ARCH_TRAP_ECALL_FROM_U;
    return ecode & ARCH_TRAP_CODE_MASK;
}

uint64_t arch_trap_epc(void) {
    return la_csr_read(LA_CSR_ERA);
}

void arch_trap_set_epc(uint64_t epc) {
    la_csr_write(LA_CSR_ERA, epc);
}

uint64_t arch_trap_tval(void) {
    return la_csr_read(LA_CSR_BADV);
}

uint64_t arch_mmu_token_from_pgdir(uint64_t *pgdir) {
    return (uint64_t)(uintptr_t)pgdir;
}

uint64_t arch_task_status_kernel_default(void) {
    return LA_CRMD_IE;
}

void arch_set_current_task_ptr(void *task) {
    __asm__ volatile("move $tp, %0" :: "r"(task));
}

void arch_sync_icache(void) {
    __asm__ volatile("ibar 0" ::: "memory");
}

void arch_cpu_wait(void) {
    __asm__ volatile("idle 0");
}

void arch_cpu_relax(void) {
    __asm__ volatile("nop");
}

void arch_mb(void) {
    __asm__ volatile("dbar 0" ::: "memory");
}

void arch_rmb(void) {
    __asm__ volatile("dbar 0" ::: "memory");
}

void arch_wmb(void) {
    __asm__ volatile("dbar 0" ::: "memory");
}

uint64_t arch_timer_counter(void) {
    /* In bringup path we only need a monotonic-ish value. */
    return la_csr_read(LA_CSR_TVAL);
}

void arch_timer_program(uint64_t deadline) {
    (void)deadline;
    /* One-shot enable, minimal placeholder for bringup. */
    la_csr_write(LA_CSR_TCFG, 0x1UL);
}

void arch_uart_init_hw(void) {
    g_uart_ready = 0;
    for (size_t i = 0; i < sizeof(g_uart_ports) / sizeof(g_uart_ports[0]); i++) {
        if (uart_try_port(&g_uart_ports[i]) == 0) {
            g_uart = g_uart_ports[i];
            g_uart_ready = 1;
            return;
        }
    }
}

void arch_uart_putc_hw(char c) {
    if (!g_uart_ready) arch_uart_init_hw();
    if (!g_uart_ready) return;

    for (uint32_t i = 0; i < 200000; i++) {
        if (reg_read(g_uart.base, g_uart.shift, UART_LSR) & UART_LSR_TX_IDLE) {
            reg_write(g_uart.base, g_uart.shift, UART_THR, (uint8_t)c);
            return;
        }
        arch_cpu_relax();
    }
}

int arch_uart_try_getc_hw(void) {
    if (!g_uart_ready) return -1;
    if (!(reg_read(g_uart.base, g_uart.shift, UART_LSR) & UART_LSR_RX_READY)) return -1;
    return (int)reg_read(g_uart.base, g_uart.shift, UART_RHR);
}
