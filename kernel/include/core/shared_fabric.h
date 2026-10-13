#ifndef A20_CORE_SHARED_FABRIC_H
#define A20_CORE_SHARED_FABRIC_H
#include "core/types.h"

/* A20 shared-memory fabric ABI v1. This is independent of CPU ISA, board,
 * physical bus and CPU-private storage. Callbacks live in immutable memory
 * and must work before the boot CPU initializes shared data/BSS. Values are
 * host-endian; a bus adapter performs any required byte conversion. Shared
 * RAM must be uncached or hardware coherent on EVERY participating CPU.
 * A barrier callback alone cannot make private CPU caches coherent. */
#define A20_FABRIC_MAGIC 0x41323053U
#define A20_FABRIC_MAX_CPUS 16U
#define A20_FABRIC_ATOMIC_GATE 0U
#define A20_FABRIC_HEAP_GATE 1U
#define A20_FABRIC_GATE_COUNT 2U
enum {
    A20_FABRIC_REG_MAGIC = 0x00,
    A20_FABRIC_REG_CPU_ID = 0x04,
    A20_FABRIC_REG_CPU_COUNT = 0x08,
    A20_FABRIC_REG_CLAIM = 0x0c,
    A20_FABRIC_REG_RELEASE = 0x10,
    A20_FABRIC_REG_FAULT = 0x14,
    A20_FABRIC_REG_ATTACHED = 0x18,
    A20_FABRIC_REG_NOTIFY = 0x20,
    A20_FABRIC_REG_ACK = 0x24,
    A20_FABRIC_REG_GATES = 0x100,
};

typedef struct a20_fabric_io {
    uint16_t (*read16)(void *ctx, unsigned offset);
    uint32_t (*read32)(void *ctx, unsigned offset);
    void (*write16)(void *ctx, unsigned offset, uint16_t value);
    /* Full device/shared-memory ordering, including posted-write completion. */
    void (*barrier)(void *ctx);
    void (*relax)(void *ctx);
    uintptr_t (*irq_save)(void *ctx);
    void (*irq_restore)(void *ctx, uintptr_t token);
} a20_fabric_io_t;

typedef struct a20_fabric {
    const a20_fabric_io_t *io;
    void *ctx; /* Immutable or CPU-private; never another CPU's private RAM. */
} a20_fabric_t;

/* Return zero or the boot fault code. Missing magic/invalid local arguments
 * return 1 without writing FAULT. Other rejections latch the returned code.
 * ATTACHED/CLAIM are destructive reads; probe_attach is called once per reset. */
unsigned a20_fabric_probe_attach(const a20_fabric_t *f, unsigned expected,
                                 unsigned *cpu);
unsigned a20_fabric_claim(const a20_fabric_t *f);
int a20_fabric_wait_release(const a20_fabric_t *f, unsigned cpu);
int a20_fabric_release(const a20_fabric_t *f, unsigned cpu);
int a20_fabric_notify(const a20_fabric_t *f, uint32_t mask);
/* -1 means a fenced domain, 0 none, 1 consumed notification. */
int a20_fabric_take_notification(const a20_fabric_t *f);
int a20_fabric_gate_lock(const a20_fabric_t *f, unsigned gate,
                         uintptr_t *irq_token);
int a20_fabric_gate_unlock(const a20_fabric_t *f, unsigned gate,
                           uintptr_t irq_token);
void a20_fabric_latch_fault(const a20_fabric_t *f, uint16_t code);

#ifdef CONFIG_SHARED_FABRIC_ATOMICS
/* Binding hooks for the portable GCC atomic runtime. The current CPU must
 * select its own port; no heap, locks or compiler atomics inside these hooks.
 * On a fenced domain the stop hook must never resume shared-memory access. */
const a20_fabric_t *arch_shared_fabric(void);
void arch_shared_fabric_stop(void) __attribute__((noreturn));
#endif
#endif
