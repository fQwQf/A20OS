#include "core/shared_fabric.h"

static int usable(const a20_fabric_t *f) {
    return f && f->io && f->io->read16 && f->io->read32 && f->io->write16 &&
           f->io->barrier && f->io->relax;
}

void a20_fabric_latch_fault(const a20_fabric_t *f, uint16_t code) {
    f->io->write16(f->ctx, A20_FABRIC_REG_FAULT, code);
    f->io->barrier(f->ctx);
}

unsigned a20_fabric_probe_attach(const a20_fabric_t *f, unsigned expected,
                                 unsigned *cpu) {
    if (!usable(f) || !cpu || expected < 2 || expected > A20_FABRIC_MAX_CPUS ||
        f->io->read32(f->ctx, A20_FABRIC_REG_MAGIC) != A20_FABRIC_MAGIC)
        return 1;
    unsigned id = f->io->read16(f->ctx, A20_FABRIC_REG_CPU_ID);
    if (id >= expected ||
        f->io->read16(f->ctx, A20_FABRIC_REG_CPU_COUNT) != expected) {
        a20_fabric_latch_fault(f, 4);
        return 4;
    }
    if (f->io->read16(f->ctx, A20_FABRIC_REG_ATTACHED) != 1) {
        a20_fabric_latch_fault(f, 5);
        return 5;
    }
    *cpu = id;
    return 0;
}

unsigned a20_fabric_claim(const a20_fabric_t *f) {
    if (f->io->read16(f->ctx, A20_FABRIC_REG_CLAIM) == 1)
        return 0;
    a20_fabric_latch_fault(f, 6);
    return 6;
}

int a20_fabric_wait_release(const a20_fabric_t *f, unsigned cpu) {
    if (cpu >= A20_FABRIC_MAX_CPUS)
        return -1;
    for (;;) {
        if (f->io->read16(f->ctx, A20_FABRIC_REG_FAULT))
            return -1;
        if (f->io->read16(f->ctx, A20_FABRIC_REG_RELEASE) & (1U << cpu)) {
            f->io->barrier(f->ctx);
            return 0;
        }
        f->io->relax(f->ctx);
    }
}

int a20_fabric_notify(const a20_fabric_t *f, uint32_t mask) {
    if (!mask || mask >> A20_FABRIC_MAX_CPUS ||
        f->io->read16(f->ctx, A20_FABRIC_REG_FAULT))
        return -1;
    unsigned count = f->io->read16(f->ctx, A20_FABRIC_REG_CPU_COUNT);
    if (count < 2 || count > A20_FABRIC_MAX_CPUS || mask >> count)
        return -1;
    f->io->barrier(f->ctx);
    f->io->write16(f->ctx, A20_FABRIC_REG_NOTIFY, (uint16_t)mask);
    return 0;
}

int a20_fabric_release(const a20_fabric_t *f, unsigned cpu) {
    if (!cpu || cpu >= A20_FABRIC_MAX_CPUS ||
        f->io->read16(f->ctx, A20_FABRIC_REG_FAULT))
        return -1;
    unsigned count = f->io->read16(f->ctx, A20_FABRIC_REG_CPU_COUNT);
    if (count < 2 || count > A20_FABRIC_MAX_CPUS || cpu >= count)
        return -1;
    f->io->barrier(f->ctx);
    f->io->write16(f->ctx, A20_FABRIC_REG_RELEASE, (uint16_t)(1U << cpu));
    return 0;
}

int a20_fabric_take_notification(const a20_fabric_t *f) {
    if (f->io->read16(f->ctx, A20_FABRIC_REG_FAULT))
        return -1;
    return f->io->read16(f->ctx, A20_FABRIC_REG_ACK) != 0;
}

int a20_fabric_gate_lock(const a20_fabric_t *f, unsigned gate,
                         uintptr_t *irq_token) {
    if (gate >= A20_FABRIC_GATE_COUNT || !irq_token || !f->io->irq_save ||
        !f->io->irq_restore)
        return -1;
    uintptr_t token = f->io->irq_save(f->ctx);
    for (;;) {
        if (f->io->read16(f->ctx, A20_FABRIC_REG_FAULT)) {
            f->io->irq_restore(f->ctx, token);
            return -1;
        }
        if (f->io->read16(f->ctx, A20_FABRIC_REG_GATES + gate * 4) == 1)
            break;
        f->io->relax(f->ctx);
    }
    f->io->barrier(f->ctx);
    *irq_token = token;
    return 0;
}

int a20_fabric_gate_unlock(const a20_fabric_t *f, unsigned gate,
                           uintptr_t irq_token) {
    if (gate >= A20_FABRIC_GATE_COUNT ||
        f->io->read16(f->ctx, A20_FABRIC_REG_FAULT))
        return -1;
    f->io->barrier(f->ctx);
    f->io->write16(f->ctx, A20_FABRIC_REG_GATES + gate * 4, 1);
    f->io->barrier(f->ctx);
    f->io->irq_restore(f->ctx, irq_token);
    return 0;
}
