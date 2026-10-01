/*
 * Shared RISC-V PLIC controller.
 *
 * Register offsets follow the PLIC specification (SiFive PLIC / Zurich).  The
 * one part that board copies historically got wrong is the per-hart enable
 * register: it is an array of 32-bit words covering 32 interrupts each, so
 * interrupt N lives in word N/32 at bit N%32.  Two of the three in-tree board
 * copies indexed it as a single word (1U << irq), which silently truncated any
 * interrupt number above 31; the correct form is used here for every board.
 *
 * The offsets are deliberately named PLIC_OFF_* rather than reusing the
 * PLIC_* names that each arch's include/platform.h already owns: this file must
 * not depend on which architecture is being built.
 *
 * See kernel/include/drivers/irqchip/plic.h for the contract.
 */

#include "drivers/irqchip/plic.h"

#include "core/cpu.h"

/* PLIC register offsets from the controller base. */
#define PLIC_OFF_PRIORITY      0x0000UL
#define PLIC_OFF_PENDING       0x1000UL
#define PLIC_OFF_SENABLE(h)    (0x2080UL + (uint64_t)(h) * 0x100UL)
#define PLIC_OFF_SPRIORITY(h)  (0x201000UL + (uint64_t)(h) * 0x2000UL)
#define PLIC_OFF_SCLAIM(h)     (0x201004UL + (uint64_t)(h) * 0x2000UL)

/* Words in one hart's enable block; each covers 32 interrupts. */
#define PLIC_ENABLE_WORDS  32U

static uintptr_t       g_plic_base;
static plic_hart_id_fn g_plic_hart_id;

static inline uint64_t plic_hart(void) {
    /* Fall back to the logical cpu id when a board did not supply a resolver,
     * which is correct on single-hart boards. */
    return g_plic_hart_id ? g_plic_hart_id() : (uint64_t)cpu_current_id();
}

/* Offset arithmetic stays in uintptr_t so that a 32-bit target does not promote
 * the sum to 64 bits and then narrow it back with a diagnostic. */
static inline volatile uint32_t *plic_at(uint64_t off) {
    uintptr_t addr = g_plic_base + (uintptr_t)off;
    return (volatile uint32_t *)addr;
}

void plic_configure(uintptr_t base, plic_hart_id_fn hart_id) {
    g_plic_base    = base;
    g_plic_hart_id = hart_id;
}

uintptr_t plic_base(void) {
    return g_plic_base;
}

static void plic_init(void) {
    uint64_t hart = plic_hart();
    /* Clear any enable/priority state left behind by firmware so a soft reset
     * cannot re-arm an interrupt the kernel has not claimed yet. */
    for (unsigned i = 0; i < PLIC_ENABLE_WORDS; i++)
        *plic_at(PLIC_OFF_SENABLE(hart) + i * sizeof(uint32_t)) = 0;
    *plic_at(PLIC_OFF_SPRIORITY(hart)) = 0;
}

static void plic_enable_irq(uint32_t irq) {
    uint32_t word = irq / 32U;
    uint32_t bit  = irq % 32U;

    /* Interrupts beyond the first block are not wired on any board here. */
    if (word >= PLIC_ENABLE_WORDS)
        return;
    *plic_at(PLIC_OFF_SENABLE(plic_hart()) + word * sizeof(uint32_t)) |= (1U << bit);
    *plic_at(PLIC_OFF_PRIORITY + (uint64_t)irq * 4) = 1;
}

static void plic_disable_irq(uint32_t irq) {
    uint32_t word = irq / 32U;
    uint32_t bit  = irq % 32U;

    if (word >= PLIC_ENABLE_WORDS)
        return;
    *plic_at(PLIC_OFF_SENABLE(plic_hart()) + word * sizeof(uint32_t)) &= ~(1U << bit);
}

/* Claim and completion run in the arch trap path, which reads the claim
 * register itself.  These callbacks exist so driver_irq_dispatch() can
 * optional-call ack/eoi without those calls having side effects. */
static uint32_t plic_ack(void) {
    return 0;
}

static void plic_eoi(uint32_t irq) {
    (void)irq;
}

const irqchip_ops_t plic_irqchip_ops = {
    .init        = plic_init,
    .enable_irq  = plic_enable_irq,
    .disable_irq = plic_disable_irq,
    .ack         = plic_ack,
    .eoi         = plic_eoi,
};

uint32_t plic_claim(void) {
    return *plic_at(PLIC_OFF_SCLAIM(plic_hart()));
}

void plic_complete(uint32_t irq) {
    /* On a RISC-V PLIC, reading SCLAIM gates the source off; writing the id back
     * is what lets it pend again.  That write-back is the completion, and it is
     * what kernel/arch/riscv64/trap/irqchip.c performs inline today. */
    *plic_at(PLIC_OFF_SCLAIM(plic_hart())) = irq;
}

uint32_t plic_pending(void) {
    return *plic_at(PLIC_OFF_PENDING);
}
