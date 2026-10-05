/*
 * Guest device model: the MMIO half of the second-stage fault handler.  The
 * frozen contract is kernel/include/hyp/hyp_vcpu.h (v2); the design record is
 * docs/hypervisor/00-design.md.
 *
 * ONE MMIO ACCESS AT A TIME, NO LOCK.  hyp_dev_mmio() runs in the guest's trap
 * path -- the guest is stopped in HS-mode with a trap frame live, and the
 * handler that called it is on the way to sret.  Anything taken here must not
 * block and must not need a lock: a lock would be held across the return into
 * guest execution, and a sleep would never be woken on this path.  So every
 * piece of shared state below is reached through __atomic ops, and the guest's
 * own UART never waits for anything.
 *
 * WHAT IS MODELLED, AND WHY SO LITTLE.  The guest is a real kernel, not a
 * device-test program: its boot walks a DTB, sizes the CLINT, initializes a
 * PLIC, and enables interrupts it expects to be delivered.  A second-stage
 * fault on a register nobody implements must therefore NOT be fatal or it
 * wedges before the guest's first banner -- but silently answering everything
 * would also let a guest spin forever on a frozen clock.  So: the two regions
 * the guest cannot boot without (UART, CLINT time) are modelled at register
 * level, everything else is RAZ/WI and tallied, and interrupts are not routed
 * here at all (they are delegated -- see the contract's hideleg).
 */
#include "hyp/hyp_vcpu.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "core/string.h"
#include "core/timer.h"
#include "core/errno.h"
#include "mm/pt.h"

/* qemu-virt's map, from the platform spec (docs/hypervisor/00-design.md S5):
 * a 16550 at 0x10000000 and the CLINT at 0x02000000.  Only these two ranges
 * are modelled; the region above them is memory-mapped devices the guest may
 * probe, which is the RAZ/WI path. */
#define HYP_UART_BASE  0x10000000ULL
#define HYP_UART_SIZE  0x00001000ULL
#define HYP_CLINT_BASE 0x02000000ULL
#define HYP_CLINT_SIZE 0x00010000ULL

/* 16550 register file, at byte offsets inside the UART page. */
#define HYP_UART_THR    0x0   /* write: transmit; read: RBR, no input here */
#define HYP_UART_IER    0x1
#define HYP_UART_FCR_IIR 0x2  /* write: FCR; read: IIR */
#define HYP_UART_LCR    0x3
#define HYP_UART_MCR    0x4
#define HYP_UART_LSR    0x5
#define HYP_UART_MSR    0x6
#define HYP_UART_SCR    0x7

/* LSR as a 16550 with an always-empty holding register reports: THRE (bit 5,
 * holding register empty) and TEMT (bit 6, shift register empty).  The
 * contract fixes this value, and it is the one LSR bit a guest's polled
 * console driver loops on, so a guest printing a character never waits. */
#define HYP_UART_LSR_TX_EMPTY 0x60
/* IIR: bit 0 low means "an interrupt is pending".  Nothing here ever raises
 * one -- the guest's UART interrupts are delegated, not injected by this
 * model -- so the honest read is "no interrupt pending" (0x01).  RAZ here
 * would claim a pending interrupt that will never be delivered, which is the
 * one value that can wedge a driver that polls IIR. */
#define HYP_UART_IIR_NONE    0x01
/* MSR: DSR and CTS asserted, so a driver that waits for carrier before
 * writing does not wait forever.  Bit 7 (RLSD) stays low: loopback is off. */
#define HYP_UART_MSR_CARRIER 0x30

/* CLINT registers (SiFive CLINT memory map).  mtime is the one the contract
 * requires: a guest whose time source does not move spins forever in its
 * delay loops, so it has to be the HOST's moving counter. */
#define HYP_CLINT_MSIP        0x0000
#define HYP_CLINT_MTIMECMP_LO 0x4000
#define HYP_CLINT_MTIMECMP_HI 0x4004
#define HYP_CLINT_MTIME_LO    0xbff8
#define HYP_CLINT_MTIME_HI    0xbffc

/* ---- unmapped-MMIO tally ----
 * A fixed open-addressed table rather than a per-VM map: the point of the
 * counter is to make a guest's device probing visible in a log without ever
 * being able to block it, so it must not allocate, must not lock, and must
 * not need the VM walked.  Collisions merge two pages' counts; that is fine
 * for a diagnostic. */
#define HYP_RAZ_SLOTS 64
#define HYP_RAZ_LOG_EVERY 4096
static uint64_t g_hyp_raz_hits[HYP_RAZ_SLOTS];

static void hyp_dev_note_unknown(uint64_t gpa, int len)
{
    uint64_t slot = (gpa >> PAGE_SIZE_BITS) & (HYP_RAZ_SLOTS - 1);
    uint64_t n = __atomic_fetch_add(&g_hyp_raz_hits[slot],
                                   len > 0 ? (uint64_t)len : 1,
                                   __ATOMIC_RELAXED);
    /* One line per HYP_RAZ_LOG_EVERY accesses on a page, so a guest polling a
     * device that does not exist says so once in a while instead of never --
     * and never often enough to drown the console the smoke reads. */
    if ((n + 1) % HYP_RAZ_LOG_EVERY == 0)
        kwarn("hyp: guest MMIO at %lx x%lu times (no model)\n",
              (unsigned long)gpa, (unsigned long)((n + 1) / HYP_RAZ_LOG_EVERY));
}

/* ---- guest console: byte count and marker match ---- */

/*
 * One guest UART byte, on its way to the host console.  The counter and the
 * marker cursor are per-VM and touched from here with no lock held, so both
 * go through atomics; the byte payload itself does not, since only the CPU
 * running the guest can produce it.
 *
 * The match is a restart-on-first-mismatch sliding window: a byte that
 * begins the marker again keeps one byte of progress, everything else restarts.
 * That finds every non-overlapping occurrence of a banner; it can miss a
 * self-overlapping one ("AA" inside a marker that starts "AA"), which a banner
 * does not contain and which a KMP automaton would cost more state to catch.
 */
static void hyp_dev_console_byte(hyp_vm_t *vm, uint8_t ch)
{
    __atomic_fetch_add(&vm->console_bytes, 1, __ATOMIC_RELAXED);

    /* Acquire against the release store of marker_len in hyp_vm_set_marker():
     * the bytes must be visible before the length says they exist. */
    uint32_t len = __atomic_load_n(&vm->marker_len, __ATOMIC_ACQUIRE);
    if (len == 0 || len > HYP_VM_MARKER_MAX)
        return;
    if (__atomic_load_n(&vm->marker_seen, __ATOMIC_ACQUIRE))
        return;     /* one hit answers the question; stop matching */

    uint32_t pos = __atomic_load_n(&vm->marker_pos, __ATOMIC_RELAXED);
    if (pos >= len)
        pos = 0;

    if ((uint8_t)vm->marker[pos] == ch) {
        pos++;
        if (pos == len) {
            __atomic_store_n(&vm->marker_pos, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&vm->marker_seen, 1, __ATOMIC_RELEASE);
            return;
        }
    } else {
        pos = (ch == (uint8_t)vm->marker[0]) ? 1 : 0;
    }
    __atomic_store_n(&vm->marker_pos, pos, __ATOMIC_RELAXED);
}

void hyp_vm_set_marker(hyp_vm_t *vm, const char *marker)
{
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return;

    uint32_t n = 0;
    if (marker) {
        while (n < HYP_VM_MARKER_MAX && marker[n])
            n++;
    }
    /* Clear first, so a shorter marker cannot leave a tail of the previous one
     * that a re-armed match could still be walking. */
    memset(vm->marker, 0, sizeof(vm->marker));
    for (uint32_t i = 0; i < n; i++)
        vm->marker[i] = marker[i];

    __atomic_store_n(&vm->marker_pos, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&vm->marker_seen, 0, __ATOMIC_RELAXED);
    /* Release: everything above is visible to a reader that acquires this. */
    __atomic_store_n(&vm->marker_len, n, __ATOMIC_RELEASE);
}

int hyp_vm_marker_seen(hyp_vm_t *vm)
{
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return 0;
    return __atomic_load_n(&vm->marker_seen, __ATOMIC_ACQUIRE);
}

uint64_t hyp_vm_console_bytes(hyp_vm_t *vm)
{
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return 0;
    return __atomic_load_n(&vm->console_bytes, __ATOMIC_RELAXED);
}

/* ---- UART ---- */

static uint8_t hyp_uart_read_reg(uint64_t off)
{
    switch (off) {
    case HYP_UART_FCR_IIR: return HYP_UART_IIR_NONE;
    case HYP_UART_LSR:     return HYP_UART_LSR_TX_EMPTY;
    case HYP_UART_MSR:     return HYP_UART_MSR_CARRIER;
    case HYP_UART_THR:     return 0;   /* RBR: this model has no input */
    default:               return 0;   /* IER/LCR/MCR/SCR: RAZ */
    }
}

static void hyp_uart_write_reg(hyp_vm_t *vm, uint64_t off, uint8_t v)
{
    if (off == HYP_UART_THR) {
        putchar((char)v);
        hyp_dev_console_byte(vm, v);
    }
    /* Everything else stores silently: the 16550 accepts a configuration the
     * model does not need to honour (divisors, FIFOs, modem control), and
     * refusing to accept the store would only turn a harmless register into a
     * guest trap. */
}

/* ---- CLINT ---- */

/*
 * The host's timebase, read straight from the hardware counter.  On this
 * platform timer_get_ticks() is `csrr time` (kernel/arch/riscv64/platform/
 * timer.c), which IS the counter the CLINT publishes as mtime -- the host's
 * 10 MHz timebase, in the guest's expected tick unit.  The monotonic
 * sec/ns pair from core/timekeeping.h would measure the same instant but in
 * the wrong unit: mtime is compared against mtimecmp and against the guest's
 * own tick arithmetic, so converting through nanoseconds only buys rounding
 * at the point where the guest wants an exact counter.
 */
static uint64_t hyp_dev_host_mtime(void)
{
    return timer_get_ticks();
}

/* off is the absolute offset in the CLINT page, already advanced per byte. */
static uint8_t hyp_clint_read_reg(uint64_t off, int byte_index)
{
    switch (off) {
    case HYP_CLINT_MTIME_LO:
        return (uint8_t)((hyp_dev_host_mtime() >> (8 * byte_index)) & 0xff);
    case HYP_CLINT_MTIME_HI:
        return (uint8_t)((hyp_dev_host_mtime() >> (32 + 8 * byte_index)) & 0xff);
    default:
        /* MSIP reads zero (no software interrupts), mtimecmp reads zero
         * (nothing programmed), the performance counters read zero. */
        return 0;
    }
}

static void hyp_clint_write_reg(uint64_t off, uint8_t v)
{
    /* Everything is discarded, including mtimecmp.  A guest that arms mtimecmp
     * and expects the timer interrupt to arrive needs the interrupt half of
     * the model, which this slice delegates to the guest's own tvec instead of
     * injecting (hyp_vcpu.h: hideleg = HYP_HIDELEG_DEFAULT); until that lands,
     * honouring the compare would raise interrupts nobody has a handler for.
     * Discarding keeps the guest's own polling paths working. */
    (void)off;
    (void)v;
}

/* ---- dispatch ---- */

/* Value bits an access of len bytes actually carries; a read must not hand the
 * guest register whatever was above them. */
static uint64_t hyp_dev_len_mask(int len)
{
    if (len >= 8)
        return ~(uint64_t)0;
    if (len <= 0)
        return 0;
    return (((uint64_t)1 << (len * 8)) - 1);
}

/*
 * One register-array access, decomposed into bytes.  A guest reads the LSR
 * inside a 32-bit load more often than it reads it alone (it reads IER, LSR
 * and MSR of the same word), so composing the answer byte by byte is what
 * makes a wide access return the same register values a narrow one would.
 */
static void hyp_dev_access(hyp_vm_t *vm, uint64_t off,
                           int store, uint64_t *value, int len,
                           uint8_t (*rd)(void *, uint64_t, int),
                           void (*wr)(void *, uint64_t, uint8_t))
{
    uint64_t v = store ? *value : 0;
    for (int i = 0; i < len; i++) {
        uint64_t a = off + (uint64_t)i;
        uint8_t b = (uint8_t)((v >> (8 * i)) & 0xff);
        if (store) {
            wr(vm, a, b);
        } else {
            uint8_t r = rd(vm, a, i);
            v = (v & ~((uint64_t)0xff << (8 * i))) | ((uint64_t)r << (8 * i));
        }
    }
    if (!store)
        *value = v & hyp_dev_len_mask(len);
}

static uint8_t hyp_uart_rd(void *vm, uint64_t off, int i)
{
    (void)vm;
    (void)i;
    return hyp_uart_read_reg(off);
}

static void hyp_uart_wr(void *vm, uint64_t off, uint8_t v)
{
    hyp_uart_write_reg((hyp_vm_t *)vm, off, v);
}

static uint8_t hyp_clint_rd(void *vm, uint64_t off, int i)
{
    (void)vm;
    return hyp_clint_read_reg(off, i);
}

static void hyp_clint_wr(void *vm, uint64_t off, uint8_t v)
{
    (void)vm;
    hyp_clint_write_reg(off, v);
}

int hyp_dev_mmio(hyp_vm_t *vm, uint64_t gpa, int store, uint64_t *value, int len)
{
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return 0;
    if (!value || len <= 0 || len > 8)
        return 0;   /* a caller this broken is a host bug, not a guest one */

    /* The RAM window is the fill path's, not this model's.  Answering 0 leaves
     * the GPA to the caller as HYP_EXIT_FAULT, which is what a fill that ran
     * out of memory should look like; a silent RAZ here would hide an
     * exhausted guest's memory behind an ignored store. */
    if (hyp_vm_ram_contains(vm, gpa))
        return 0;

    /* Both windows are tested for the WHOLE access, not just its first byte.
     * hyp_dev_access() decomposes into bytes and walks off + i, so an 8-byte
     * access whose first byte is the last of a window would otherwise keep
     * synthesising bytes from whatever address follows -- composing part of a
     * device with part of the next window, or with the RAZ default.  A guest
     * doing an unaligned wide access that straddles the edge gets a plain
     * fault instead, which is also what real hardware does to an access that
     * crosses a device boundary. */
    if (gpa >= HYP_UART_BASE && gpa - HYP_UART_BASE < HYP_UART_SIZE &&
        (uint64_t)len <= HYP_UART_SIZE - (gpa - HYP_UART_BASE)) {
        hyp_dev_access(vm, gpa - HYP_UART_BASE, store, value, len,
                       hyp_uart_rd, hyp_uart_wr);
        return 1;
    }
    if (gpa >= HYP_CLINT_BASE && gpa - HYP_CLINT_BASE < HYP_CLINT_SIZE &&
        (uint64_t)len <= HYP_CLINT_SIZE - (gpa - HYP_CLINT_BASE)) {
        uint64_t off = gpa - HYP_CLINT_BASE;
        /* An 8-byte load of mtime is what a guest that believes the register
         * is 64-bit aligned issues.  The real mtime straddles 0xBFF8/0xBFFC,
         * so hardware would split it too, but composing eight bytes one at a
         * time would hand back half a counter and four zeros -- the whole
         * word goes out in one piece instead. */
        if (!store && len == 8 && off == HYP_CLINT_MTIME_LO) {
            *value = hyp_dev_host_mtime();
            return 1;
        }
        hyp_dev_access(vm, off, store, value, len, hyp_clint_rd, hyp_clint_wr);
        return 1;
    }

    /* No model for this page: RAZ/WI, tallied, and the guest resumes.  A guest
     * probing a device that is not there has to be able to finish probing. */
    hyp_dev_note_unknown(gpa, len);
    if (!store)
        *value = 0;
    return 1;
}