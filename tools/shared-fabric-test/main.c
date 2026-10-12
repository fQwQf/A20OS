/* Exercise the production common driver through an ISA-neutral fake bus.
 * This tests software semantics, not a Raspberry Pi/FPGA/board controller. */
#include "core/shared_fabric.h"
extern int puts(const char *);
extern void exit(int) __attribute__((noreturn));
#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            puts("FAIL: " #x);                                                 \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

typedef struct domain {
    uint32_t magic;
    unsigned count, attached, claimed, released, notifications, fault;
    unsigned owner[2], barriers, writes;
} domain_t;
typedef struct port {
    domain_t *d;
    unsigned id, relaxes;
    uintptr_t irq;
} port_t;

static uint32_t read32(void *ctx, unsigned off) {
    port_t *p = ctx;
    CHECK(off == A20_FABRIC_REG_MAGIC);
    return p->d->magic;
}
static uint16_t read16(void *ctx, unsigned off) {
    port_t *p = ctx;
    domain_t *d = p->d;
    switch (off) {
        case A20_FABRIC_REG_CPU_ID:
            return p->id;
        case A20_FABRIC_REG_CPU_COUNT:
            return d->count;
        case A20_FABRIC_REG_FAULT:
            return d->fault;
        case A20_FABRIC_REG_RELEASE:
            return d->released;
        case A20_FABRIC_REG_ATTACHED:
            if (d->attached & (1U << p->id))
                return 0;
            d->attached |= 1U << p->id;
            return 1;
        case A20_FABRIC_REG_CLAIM:
            if (p->id || d->claimed)
                return 0;
            d->claimed = 1;
            return 1;
        case A20_FABRIC_REG_ACK: {
            unsigned result = d->notifications & (1U << p->id);
            d->notifications &= ~(1U << p->id);
            return result != 0;
        }
        default: {
            CHECK(off == A20_FABRIC_REG_GATES ||
                  off == A20_FABRIC_REG_GATES + 4);
            unsigned gate = (off - A20_FABRIC_REG_GATES) / 4;
            if (d->owner[gate])
                return 0;
            d->owner[gate] = p->id + 1;
            return 1;
        }
    }
}
static void write16(void *ctx, unsigned off, uint16_t value) {
    port_t *p = ctx;
    domain_t *d = p->d;
    d->writes++;
    switch (off) {
        case A20_FABRIC_REG_RELEASE:
            d->released |= value;
            break;
        case A20_FABRIC_REG_NOTIFY:
            d->notifications |= value;
            break;
        case A20_FABRIC_REG_FAULT:
            if (!d->fault)
                d->fault = value;
            break;
        default: {
            CHECK(off == A20_FABRIC_REG_GATES ||
                  off == A20_FABRIC_REG_GATES + 4);
            unsigned gate = (off - A20_FABRIC_REG_GATES) / 4;
            CHECK(value == 1 && d->owner[gate] == p->id + 1);
            d->owner[gate] = 0;
            break;
        }
    }
}
static void barrier(void *ctx) {
    ((port_t *)ctx)->d->barriers++;
}
static void relax(void *ctx) {
    port_t *p = ctx;
    p->relaxes++;
    /* Complete another port's previously granted operation. */
    p->d->owner[0] = 0;
    p->d->released |= 1U << p->id;
}
static uintptr_t irq_save(void *ctx) {
    port_t *p = ctx;
    uintptr_t old = p->irq;
    p->irq = 0;
    return old;
}
static void irq_restore(void *ctx, uintptr_t old) {
    ((port_t *)ctx)->irq = old;
}
static const a20_fabric_io_t io = {
    .read16 = read16,
    .read32 = read32,
    .write16 = write16,
    .barrier = barrier,
    .relax = relax,
    .irq_save = irq_save,
    .irq_restore = irq_restore,
};

#ifdef CONFIG_SHARED_FABRIC_ATOMICS
static const a20_fabric_t *selected_fabric;
const a20_fabric_t *arch_shared_fabric(void) {
    return selected_fabric;
}
void arch_shared_fabric_stop(void) {
#ifdef A20_TEST_ATOMIC_FAULT
    port_t *p = selected_fabric->ctx;
    CHECK(p->d->fault == 5 && !p->d->owner[0] && p->irq == ~(uintptr_t)0);
    puts("PASS atomic fault: stopped before shared access, IRQ token restored");
    exit(0);
#endif
    puts("unexpected atomic fabric stop");
    exit(1);
}

/* Compile with -fno-inline-atomics: these are compiler-emitted runtime calls
 * into production fabric_atomics.c, not host native atomics. */
#define PROBE(T)                                                               \
    do {                                                                       \
        T value = 7, expected = 7;                                             \
        CHECK(__atomic_load_n(&value, __ATOMIC_ACQUIRE) == 7);                 \
        CHECK(__atomic_compare_exchange_n(                                     \
            &value, &expected, (T)9, 0, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE));  \
        expected = 7;                                                          \
        CHECK(!__atomic_compare_exchange_n(                                    \
            &value, &expected, (T)11, 1, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE)); \
        CHECK(expected == 9);                                                  \
        CHECK(__atomic_exchange_n(&value, (T)11, __ATOMIC_ACQ_REL) == 9);      \
        CHECK(__atomic_fetch_add(&value, (T)3, __ATOMIC_RELAXED) == 11 &&      \
              value == 14);                                                    \
        CHECK(__atomic_sub_fetch(&value, (T)2, __ATOMIC_RELAXED) == 12);       \
        CHECK(__atomic_fetch_or(&value, (T)3, __ATOMIC_RELAXED) == 12 &&       \
              value == 15);                                                    \
        CHECK(__atomic_and_fetch(&value, (T)7, __ATOMIC_RELAXED) == 7);        \
        CHECK(__atomic_fetch_xor(&value, (T)5, __ATOMIC_RELAXED) == 7 &&       \
              value == 2);                                                     \
        __atomic_store_n(&value, (T) ~(T)0, __ATOMIC_RELEASE);                 \
        CHECK(__atomic_add_fetch(&value, (T)1, __ATOMIC_SEQ_CST) == 0);        \
    } while (0)
static void atomic_probe(const a20_fabric_t *f) {
    selected_fabric = f;
    port_t *p = f->ctx;
    uintptr_t before = p->irq;
    unsigned grants = p->d->barriers;
    PROBE(uint8_t);
    PROBE(uint16_t);
    PROBE(unsigned int);
    PROBE(uint64_t);
    uint64_t carry = 0xffffffffULL, expected = 0;
    CHECK(__atomic_add_fetch(&carry, 1, __ATOMIC_SEQ_CST) == 0x100000000ULL);
    CHECK(!__atomic_compare_exchange_n(&carry, &expected, 1, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE));
    CHECK(expected == 0x100000000ULL);
    /* Every one of the 46 emitted atomic libcalls acquired and released the
     * fabric gate: one acquire + two release barriers each. */
    CHECK(p->irq == before && !p->d->owner[0] &&
          p->d->barriers - grants == 138);
}
#endif

int main(void) {
    for (unsigned count = 2; count <= 16; count++) {
        domain_t d = {.magic = A20_FABRIC_MAGIC, .count = count};
        port_t ports[16];
        a20_fabric_t f[16];
        for (unsigned cpu = 0; cpu < count; cpu++) {
            ports[cpu] = (port_t){.d = &d, .id = cpu};
            f[cpu] = (a20_fabric_t){.io = &io, .ctx = &ports[cpu]};
            unsigned id = 99;
            CHECK(a20_fabric_probe_attach(&f[cpu], count, &id) == 0 &&
                  id == cpu);
        }
        CHECK(d.attached == (1U << count) - 1);
        CHECK(a20_fabric_claim(&f[0]) == 0);
        for (unsigned cpu = 1; cpu < count; cpu++) {
            CHECK(a20_fabric_release(&f[0], cpu) == 0);
            CHECK(a20_fabric_wait_release(&f[cpu], cpu) == 0);
        }
        d.released = 0;
        CHECK(a20_fabric_wait_release(&f[1], 1) == 0 && ports[1].relaxes);
        CHECK(a20_fabric_notify(&f[0], (1U << count) - 1) == 0);
        for (unsigned cpu = 0; cpu < count; cpu++) {
            CHECK(a20_fabric_take_notification(&f[cpu]) == 1);
            CHECK(a20_fabric_take_notification(&f[cpu]) == 0);
            uintptr_t token, saved = ~(uintptr_t)0 - cpu;
            ports[cpu].irq = saved;
            d.owner[0] = 99; /* Force the common gate retry path. */
            CHECK(a20_fabric_gate_lock(&f[cpu], 0, &token) == 0);
            CHECK(token == saved && !ports[cpu].irq && d.owner[0] == cpu + 1);
            CHECK(a20_fabric_gate_unlock(&f[cpu], 0, token) == 0);
            CHECK(ports[cpu].irq == saved && !d.owner[0]);
#ifdef CONFIG_SHARED_FABRIC_ATOMICS
            atomic_probe(&f[cpu]);
#endif
        }
        unsigned writes = d.writes;
        uintptr_t token;
        CHECK(a20_fabric_release(&f[0], 16) == -1);
        CHECK(a20_fabric_notify(&f[0], 1U << 16) == -1);
        CHECK(a20_fabric_gate_lock(&f[0], 2, &token) == -1);
        CHECK(d.writes == writes && d.barriers);
        unsigned id;
        CHECK(a20_fabric_probe_attach(&f[1], count, &id) == 5 && d.fault == 5);
        CHECK(a20_fabric_notify(&f[0], 1) == -1);
        CHECK(a20_fabric_take_notification(&f[0]) == -1);
        CHECK(a20_fabric_gate_lock(&f[0], 0, &token) == -1);
        CHECK(ports[0].irq == ~(uintptr_t)0);
#ifdef A20_TEST_ATOMIC_FAULT
        selected_fabric = &f[0];
        (void)__atomic_load_n((uint32_t *)(uintptr_t)0x12345000U,
                              __ATOMIC_ACQUIRE);
        CHECK(
            0); /* Must stop without ever dereferencing the invalid address. */
#endif
        d.fault = 0;
        d.attached = 0;
        d.magic = 0;
        writes = d.writes;
        CHECK(a20_fabric_probe_attach(&f[0], count, &id) == 1);
        CHECK(!d.attached && d.writes == writes);
        d.magic = A20_FABRIC_MAGIC;
        CHECK(a20_fabric_probe_attach(&f[0], count + 1, &id) != 0);
    }
    puts("PASS shared fabric: 2..16 ports, attachment, startup, gates, IRQ "
         "tokens, notifications, fencing, GCC atomics 1/2/4/8");
    return 0;
}
