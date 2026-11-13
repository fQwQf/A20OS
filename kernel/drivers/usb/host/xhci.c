/*
 * A20OS — xHCI host controller driver.
 *
 * Refactored from the original xhci_hid.c: the controller machinery (TRB
 * rings, slot/endpoint contexts, commands, control transfers, port reset,
 * enumeration primitives) is preserved, but made instance-based and exposed
 * through usb_hcd_ops so the USB core can drive any class of device.  HID
 * protocol parsing lives in the usb-hid class driver.
 *
 * Completion delivery is per controller: probe registers the controller's own
 * INTx line and the handler drains the event ring, so the interrupt path is
 * armed before the rings are and the fallback is explicit.  a20.xhci.poll=1
 * forces the polling fallback for a board whose line the platform does not
 * route.  xhci->lock is real: it serializes the command/endpoint/event ring
 * state and the port state, and completion callbacks run with it released
 * because they re-arm through this same lock (see docs/drivers/guide/
 * lock-order.md).
 */
#include "drivers/usb/usb.h"

#include "core/bootargs.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/timer.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/pci_msi.h"
#include "drivers/bus/pci_msix.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "mm/mm.h"
#include "core/errno.h"

#define XHCI_VENDOR_INTEL             0x8086U
#define XHCI_DEVICE_PANTHER_POINT     0x1e31U
#define XHCI_FIRMWARE_TIMEOUT_MS      1000U

#define XHCI_MAX_SLOTS                32U
#define XHCI_MAX_PORTS                32U
#define XHCI_RING_TRBS                256U
#define XHCI_EVENT_TRBS               256U
#define XHCI_WAIT_LOOPS               10000000U
#define XHCI_MAX_EPS                  32U
#define XHCI_MAX_ROUTE_DEPTH          5U
/* Completion callbacks are run in batches so the controller lock is dropped
 * between them; the batch bound keeps the interrupted section bounded. */
#define XHCI_IRQ_BATCH                8U
/* How long a synchronous transfer may take end to end, published by whoever
 * takes xfer_busy and read by whoever spins for it.  One number, so the spinner
 * can never be given a shorter budget than the holder it is waiting out, which
 * is what equating two independent iteration counts used to assert. */
#define XHCI_XFER_HOLD_BUDGET_MS    500U
/* BOT can wait on backing media I/O after the endpoint has been rung. */
#define XHCI_BULK_HOLD_BUDGET_MS    5000U
/* Spin iterations between two clock reads, so the clock does not become the
 * dominant cost of the poll it bounds. */
#define XHCI_POLL_SPINS             1024U

#define XHCI_USBCMD                   0x00U
#define XHCI_USBSTS                   0x04U
#define XHCI_PAGESIZE                 0x08U
#define XHCI_CRCR                     0x18U
#define XHCI_DCBAAP                   0x30U
#define XHCI_CONFIG                   0x38U
#define XHCI_PORTSC_BASE              0x400U
#define XHCI_PORTSC_STRIDE            0x10U

#define XHCI_CMD_RUN                  (1U << 0)
#define XHCI_CMD_RESET                (1U << 1)
/* USBCMD bit 2, Interrupt Enable.  Without it the controller raises events but
 * never asserts the INTx line, so the whole completion path is silent and only
 * the polling fallback sees anything.  USBCMD is written as a whole word, so
 * every write of USBCMD has to carry this bit or it silently disarms the
 * controller. */
#define XHCI_CMD_INTE                 (1U << 2)
#define XHCI_STS_HALTED               (1U << 0)
#define XHCI_STS_FATAL                (1U << 2)
#define XHCI_STS_EINT                 (1U << 3)
#define XHCI_STS_PCD                  (1U << 4)
#define XHCI_STS_CNR                  (1U << 11)
#define XHCI_STS_HSE                  (1U << 12)

/* Runtime interrupt register block: INTR0 at +0x00 (bit 0 = IE, bit 1 = IP),
 * IMOD at +0x04, ERSTSZ at +0x08 (the number of ERST entries), ERSTBA at
 * +0x10 and ERDP at +0x18 -- all relative to the interrupter's register block,
 * which is where interrupter 0's block starts at runtime + 0x20. */
#define XHCI_INTR0_IP                 (1U << 0)
#define XHCI_INTR0_IE                 (1U << 1)
#define XHCI_INTERRUPTER_OFFSET       0x20U
#define XHCI_ERSTSZ_OFFSET            0x08U
#define XHCI_ERSTBA_OFFSET            0x10U
#define XHCI_ERDP_OFFSET              0x18U
/* ERDP bit 3 is EHB (Event Ring Handler Busy); writing it is what tells the
 * controller the driver has caught up with the event ring. */
#define XHCI_ERDP_EHB                 (1U << 3)

#define XHCI_PORT_CCS                 (1U << 0)
#define XHCI_PORT_PED                 (1U << 1)
#define XHCI_PORT_RESET               (1U << 4)
#define XHCI_PORT_POWER               (1U << 9)
#define XHCI_PORT_PRESERVE            ((0xfU << 5) | XHCI_PORT_POWER | \
                                       (3U << 14) | (7U << 25))
#define XHCI_PORT_SPEED(v)            (((v) >> 10) & 0x0fU)
#define XHCI_PORT_CHANGE_BITS         0x00fe0000U
#define XHCI_PORT_WARM_RESET          (1U << 31)

#define XHCI_TRB_CYCLE                (1U << 0)
#define XHCI_TRB_ENT                  (1U << 1)
#define XHCI_TRB_ISP                  (1U << 2)
#define XHCI_TRB_IOC                  (1U << 5)
#define XHCI_TRB_IDT                  (1U << 6)
#define XHCI_TRB_TYPE(n)              ((uint32_t)(n) << 10)
#define XHCI_TRB_DIR_IN               (1U << 16)
#define XHCI_TRB_TRT_IN               (3U << 16)
#define XHCI_TRB_TRT_OUT              (2U << 16)

#define XHCI_TRB_NORMAL               1U
#define XHCI_TRB_SETUP                2U
#define XHCI_TRB_DATA                 3U
#define XHCI_TRB_STATUS               4U
#define XHCI_TRB_LINK                 6U
#define XHCI_TRB_ENABLE_SLOT          9U
#define XHCI_TRB_DISABLE_SLOT         10U
#define XHCI_TRB_ADDRESS_DEVICE       11U
#define XHCI_TRB_CONFIGURE_ENDPOINT   12U
#define XHCI_TRB_EVALUATE_CONTEXT     13U
#define XHCI_TRB_TRANSFER_EVENT       32U
#define XHCI_TRB_COMMAND_EVENT        33U
#define XHCI_TRB_PORT_EVENT           34U

#define XHCI_CC_SUCCESS               1U
#define XHCI_CC_SHORT_PACKET          13U
#define XHCI_CC_PARAMETER_ERROR       17U

#define USB_DIR_IN                    0x80U

typedef struct xhci_trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) xhci_trb_t;

typedef struct xhci_control_trbs {
    uint64_t setup;
    uint64_t data;
} xhci_control_trbs_t;

typedef struct xhci_erst_entry {
    uint64_t address;
    uint32_t size;
    uint32_t reserved;
} __attribute__((packed)) xhci_erst_entry_t;

typedef struct xhci_ring {
    /* One 4 KiB segment, matching Linux's TRBS_PER_SEGMENT.  Some xHCs
     * prefetch a full page beyond the link TRB; a 64-TRB (1 KiB) command ring
     * followed immediately by the event ring can make the prefetch interpret
     * event TRBs as commands and raise HCE. */
    xhci_trb_t trbs[XHCI_RING_TRBS] ALIGNED(PAGE_SIZE);
    uint16_t enqueue;
    uint8_t cycle;
} xhci_ring_t;

/* A configured non-EP0 endpoint (interrupt/bulk) with its transfer ring. */
typedef struct xhci_ep {
    struct xhci_ep *next;
    void *alloc_ptr;            /* raw kmalloc base (for freeing) */
    xhci_ring_t ring;
    uint8_t slot;
    uint8_t dci;
    uint8_t addr;
    usb_urb_t *pending;         /* armed interrupt URB, or NULL */
} xhci_ep_t;

typedef struct xhci_slot_path {
    uint32_t route;
    uint8_t root_port;
    uint8_t depth;
    uint8_t speed;
    uint8_t tt_slot;
    uint8_t tt_port;
} xhci_slot_path_t;

typedef struct xhci_controller {
    void *alloc_ptr;            /* raw kmalloc base (for freeing) */
    usb_hcd_t hcd;
    uintptr_t cap;
    size_t mmio_size;
    uintptr_t op;
    uintptr_t runtime;
    uintptr_t doorbell;
    uint8_t context_size;
    uint16_t hci_version;
    uint8_t max_slots;
    uint8_t max_ports;
    uint8_t port_power_control;
    uint8_t event_cycle;
    uint16_t event_dequeue;
    uint8_t command_cycle;
    uint16_t command_enqueue;
    uint8_t running;
    uint64_t command_ring_pa;
    uint64_t crcr_start;
    uint32_t hcs2_value;
    unsigned sp_count;
    uint32_t start_sts;
    uint32_t pci_command;
    uint8_t msi_present, msi_enabled;
    uint8_t msix_present, msix_enabled;
    uint8_t fatal;
    uint8_t fatal_logged;
    /* Set while a synchronous transfer (command, control, bulk) owns the
     * event ring.  Only one may be in flight: an event that does not match
     * the waiting request is dropped by whoever reads it first, so two
     * concurrent waiters would consume each other's completions.  It is also
     * what tells the IRQ handler to acknowledge and step aside instead of
     * consuming the ring itself. */
    uint8_t xfer_busy;
    /* Wall-clock deadline the current xfer_busy holder is held to, published by
     * whoever took the flag.  A spinner reads it under the lock instead of
     * counting its own iterations, so the two sides cannot disagree about how
     * long a synchronous transfer may take. */
    uint64_t xfer_deadline;
    uint8_t irq_registered;
    int irq;
    volatile uint64_t irq_events;   /* events the IRQ handler consumed */
    volatile uint64_t poll_events;  /* events the polling fallback consumed */
    uint64_t reported_irq_events;   /* nonzero after IRQ progress was announced */
    uint64_t reported_poll_events;
    spinlock_t lock;

    /* xHCI table 6-1: these DMA structures must fit within one page each. */
    uint64_t dcbaa[XHCI_MAX_SLOTS + 1U] ALIGNED(PAGE_SIZE);
    uint8_t output_context[XHCI_MAX_SLOTS + 1U][2048] ALIGNED(PAGE_SIZE);
    uint8_t input_context[2112] ALIGNED(PAGE_SIZE);
    xhci_trb_t command_ring[XHCI_RING_TRBS] ALIGNED(PAGE_SIZE);
    xhci_trb_t event_ring[XHCI_EVENT_TRBS] ALIGNED(PAGE_SIZE);
    xhci_erst_entry_t erst ALIGNED(PAGE_SIZE);
    /* The console dump showed this array at ...5040, 64-byte aligned but not
     * page aligned.  Linux allocates the scratchpad array with
     * dma_alloc_coherent (page aligned); a controller that checks the array
     * address can raise HSE before executing the first command. */
    uint64_t scratchpad_array[32] ALIGNED(PAGE_SIZE);
    uint8_t scratchpads[32][PAGE_SIZE] ALIGNED(PAGE_SIZE);
    xhci_ring_t ep0_ring[XHCI_MAX_SLOTS + 1U];
    uint8_t control_buffer[USB_CONTROL_MAX_LENGTH] ALIGNED(PAGE_SIZE);

    xhci_ep_t *eps;
    uint8_t ep_count;
    xhci_slot_path_t slot_path[XHCI_MAX_SLOTS + 1U];
} xhci_controller_t;

_Static_assert(sizeof(xhci_trb_t) == 16, "xHCI TRB must be 16 bytes");
_Static_assert(sizeof(usb_setup_packet_t) == 8, "USB setup packet must be 8 bytes");

/* Both allocations below are handed to the controller as bus addresses: the
 * DCBAA, the scratchpad array, every context and every ring live inside them.
 * They therefore take the device so the framework can place them inside the
 * window xhci_probe() declared, and report -EOPNOTSUPP when it cannot -- a
 * controller walking rings it cannot decode is a hang, not a slow device.
 * 0 / -ENOMEM keep their previous meaning for a plain allocation failure. */
static int xhci_alloc_controller(device_t *dev, xhci_controller_t **out);
static int xhci_alloc_ep(device_t *dev, xhci_ep_t **out);

static inline uint32_t xhci_read32(uintptr_t base, uint32_t offset) {
    return readl((const volatile void *)(base + offset));
}
static inline void xhci_write32(uintptr_t base, uint32_t offset, uint32_t value) {
    writel(value, (volatile void *)(base + offset));
}
/* CRCR/DCBAAP/ERSTBA/ERDP are 64-bit registers.  On a 64-bit build they must
 * be accessed with one 64-bit MMIO operation: splitting CRCR into two 32-bit
 * writes can leave the command ring pointer at zero on controllers that do not
 * merge the halves, which is an immediate HCE before the first command. */
static inline uint64_t xhci_read64(uintptr_t base, uint32_t offset) {
#if defined(__x86_64__) || defined(__aarch64__) || \
    (defined(__riscv) && __riscv_xlen == 64)
    return *(volatile uint64_t *)(base + offset);
#else
    uint32_t low = xhci_read32(base, offset);
    uint32_t high = xhci_read32(base, offset + 4U);
    return low | ((uint64_t)high << 32);
#endif
}
static inline void xhci_write64(uintptr_t base, uint32_t offset, uint64_t value) {
#if defined(__x86_64__) || defined(__aarch64__) || \
    (defined(__riscv) && __riscv_xlen == 64)
    *(volatile uint64_t *)(base + offset) = value;
#else
    xhci_write32(base, offset, (uint32_t)value);
    xhci_write32(base, offset + 4U, (uint32_t)(value >> 32));
#endif
}

static int xhci_wait32(uintptr_t base, uint32_t offset, uint32_t mask,
                       uint32_t expected) {
    for (uint32_t i = 0; i < XHCI_WAIT_LOOPS; i++) {
        if ((xhci_read32(base, offset) & mask) == expected)
            return 0;
        arch_cpu_relax();
    }
    return -ETIMEDOUT;
}

static void xhci_ring_init(xhci_ring_t *ring) {
    memset(ring, 0, sizeof(*ring));
    ring->cycle = 1;
    ring->trbs[XHCI_RING_TRBS - 1U].parameter = va_to_pa(ring->trbs);
    ring->trbs[XHCI_RING_TRBS - 1U].control =
        XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_ENT | XHCI_TRB_CYCLE;
    arch_dma_sync_for_device(ring->trbs, sizeof(ring->trbs));
}

/* Each queued TD is one TRB. A control transfer queues separate TDs for
 * setup, optional data and status. Defer the setup's cycle bit until all of
 * its stages have been written. */
static uint64_t xhci_ring_enqueue(xhci_ring_t *ring, uint64_t parameter,
                                  uint32_t status, uint32_t control,
                                  int defer_cycle) {
    uint16_t index = ring->enqueue;
    xhci_trb_t *trb = &ring->trbs[index];
    trb->parameter = parameter;
    trb->status = status;
    trb->control = control | ((ring->cycle ^ defer_cycle) ? XHCI_TRB_CYCLE : 0U);
    arch_dma_sync_for_device(trb, sizeof(*trb));
    wmb();
    uint64_t address = va_to_pa(trb);
    index++;
    if (index == XHCI_RING_TRBS - 1U) {
        xhci_trb_t *link = &ring->trbs[index];
        link->control = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_ENT |
                        (ring->cycle ? XHCI_TRB_CYCLE : 0U);
        arch_dma_sync_for_device(link, sizeof(*link));
        ring->cycle ^= 1U;
        index = 0;
    }
    ring->enqueue = index;
    return address;
}

static void *xhci_input_context(xhci_controller_t *xhci, unsigned index) {
    /* Section 6.2.5.1: CSZ also sizes the Input Control Context. Its upper
     * 32 bytes are reserved in 64-byte mode. */
    return xhci->input_context + (size_t)index * xhci->context_size;
}
static void *xhci_output_context(xhci_controller_t *xhci, unsigned slot,
                                 unsigned index) {
    return xhci->output_context[slot] + (size_t)index * xhci->context_size;
}

/* Consume one event ring entry.  Writing ERDP with EHB after every dequeue is
 * what both tells the controller the driver caught up and clears USBSTS.IP;
 * a consumer that finds nothing to consume uses xhci_ack_event_irq() below to
 * clear it explicitly, otherwise a level-triggered INTx line would stay
 * asserted for an interrupt that carried no work. */
static int xhci_next_event(xhci_controller_t *xhci, xhci_trb_t *result) {
    xhci_trb_t *event = &xhci->event_ring[xhci->event_dequeue];
    arch_dma_sync_for_cpu(event, sizeof(*event));
    if (!!(event->control & XHCI_TRB_CYCLE) != !!xhci->event_cycle)
        return 0;
    *result = *event;
    xhci->event_dequeue++;
    if (xhci->event_dequeue == XHCI_EVENT_TRBS) {
        xhci->event_dequeue = 0;
        xhci->event_cycle ^= 1U;
    }
    xhci_write64(xhci->runtime + XHCI_INTERRUPTER_OFFSET + XHCI_ERDP_OFFSET,
                 0, va_to_pa(&xhci->event_ring[xhci->event_dequeue]) |
                 XHCI_ERDP_EHB);
    /* Flush the posted ERDP/EHB update before the producer is allowed to
     * consider the segment free. */
    (void)xhci_read64(xhci->runtime + XHCI_INTERRUPTER_OFFSET,
                      XHCI_ERDP_OFFSET);
    return 1;
}

/* Interrupter 0's INTR0.  IP is this interrupter's pending bit and is
 * write-1-to-clear; IE is read/write.  The register is written as a whole
 * word, so clearing IP has to carry IE back in or the write that acknowledges
 * the interrupt is also the one that disarms it.
 *
 * INTR0.IP -- not USBSTS -- is what a consumer tests to ask whether this
 * interrupter has work: the controller sets it when it posts an event and
 * holds the INTx line asserted for exactly as long as it stays set, and the
 * USBSTS summary bits are not the per-interrupter state (USBSTS bit 0 is the
 * read-only Controller Halted flag, and writing it back acknowledges nothing). */
static uint32_t xhci_read_intr0(xhci_controller_t *xhci) {
    return xhci_read32(xhci->runtime + XHCI_INTERRUPTER_OFFSET, 0x00U);
}

static void xhci_ack_event_irq(xhci_controller_t *xhci) {
    uint32_t iman = xhci_read_intr0(xhci);
    xhci_write32(xhci->runtime + XHCI_INTERRUPTER_OFFSET, 0x00U,
                 (iman & XHCI_INTR0_IE) | XHCI_INTR0_IP);
    (void)xhci_read32(xhci->runtime + XHCI_INTERRUPTER_OFFSET, 0x00U);
}

static int xhci_irq_pending(xhci_controller_t *xhci) {
    return (xhci_read_intr0(xhci) & XHCI_INTR0_IP) != 0;
}

static xhci_ep_t *xhci_find_ep(xhci_controller_t *xhci, uint8_t slot,
                               uint8_t dci) {
    for (xhci_ep_t *e = xhci->eps; e; e = e->next)
        if (e->slot == slot && e->dci == dci)
            return e;
    return NULL;
}

/* Take the interrupt completion out of @event, if it names one.  Called with
 * xhci->lock held; the callback itself is NOT called here, the batch runner
 * does that with the lock released. */
static unsigned xhci_collect_interrupt(xhci_controller_t *xhci,
                                       const xhci_trb_t *event,
                                       usb_urb_t **ready) {
    if (((event->control >> 10) & 0x3fU) != XHCI_TRB_TRANSFER_EVENT)
        return 0;
    xhci_ep_t *ep = xhci_find_ep(xhci, (uint8_t)(event->control >> 24),
                                 (uint8_t)((event->control >> 16) & 0x1fU));
    if (!ep || !ep->pending)
        return 0;
    usb_urb_t *urb = ep->pending;
    ep->pending = NULL;
    /* A class driver that removed its interface clears complete/ctx before it
     * frees the URB, but this endpoint's transfer ring outlives it until
     * abort_slot() runs.  Without the check below the drain would dma-sync a
     * buffer that is already gone. */
    if (!urb->complete)
        return 0;
    uint8_t cc = (uint8_t)(event->status >> 24);
    arch_dma_sync_for_cpu(urb->buf, urb->len);
    urb->status = (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET)
                      ? 0 : (cc ? -cc : -EIO);
    ready[0] = urb;
    return 1;
}

/* Run a batch of completion callbacks with xhci->lock released.  Every
 * completion re-arms its own URB, which comes straight back through
 * submit_interrupt() and takes this same non-recursive lock, so a callback
 * under the lock would deadlock.  *flags carries the caller's irqsave state so
 * the identical masking is restored on the way back in. */
static void xhci_run_completions(usb_urb_t **ready, unsigned count,
                                 xhci_controller_t *xhci, uint64_t *flags) {
    if (!count)
        return;
    spin_unlock_irqrestore(&xhci->lock, *flags);
    for (unsigned i = 0; i < count; i++)
        if (ready[i]->complete)
            ready[i]->complete(ready[i]);
    *flags = spin_lock_irqsave(&xhci->lock);
}

/* Serialise one synchronous transfer.  The flag is polled under the controller
 * lock rather than slept on, and the poll is bounded by the deadline the
 * holder published instead of by an iteration count of our own: callers reach
 * here from paths that may already hold a lock of their own (usb-storage's
 * msc_command holds its per-device mutex across the whole BOT exchange), and
 * the cost of one iteration is machine-dependent, so an unbounded count is an
 * unbounded time in someone else's critical section.  Refusing with -EBUSY is
 * a refusal, not a queue. */
static int xhci_xfer_acquire(xhci_controller_t *xhci, unsigned budget_ms) {
    uint64_t deadline = timer_get_ticks() + MS_TO_TICKS(XHCI_BULK_HOLD_BUDGET_MS);
    for (uint32_t i = 0;; i++) {
        uint64_t flags = spin_lock_irqsave(&xhci->lock);
        if (!xhci->xfer_busy) {
            xhci->xfer_busy = 1;
            xhci->xfer_deadline = timer_get_ticks() + MS_TO_TICKS(budget_ms);
            spin_unlock_irqrestore(&xhci->lock, flags);
            return 0;
        }
        uint64_t holder_deadline = xhci->xfer_deadline;
        spin_unlock_irqrestore(&xhci->lock, flags);
        arch_cpu_relax();
        if ((i & (XHCI_POLL_SPINS - 1U)) == 0) {
            uint64_t now = timer_get_ticks();
            if (now >= deadline || now >= holder_deadline)
                return -EBUSY;
        }
    }
}

static void xhci_xfer_release(xhci_controller_t *xhci) {
    uint64_t flags = spin_lock_irqsave(&xhci->lock);
    xhci->xfer_busy = 0;
    /* The handler acknowledges IP on the holder's behalf, so a transfer that
     * left the pending bit set has to clear it here. */
    xhci_ack_event_irq(xhci);
    spin_unlock_irqrestore(&xhci->lock, flags);
}

/* Consume everything the controller has published.  Called with xhci->lock
 * held, returns with it held. */
static int xhci_drain_events(xhci_controller_t *xhci, uint64_t *flags) {
    int handled = 0;
    for (;;) {
        /* Completion callbacks drop this lock. Another CPU can claim the
         * synchronous transfer while they run, so ownership must be checked
         * again before consuming another event. Otherwise this IRQ path
         * silently discards a bulk/command completion meant for that waiter. */
        if (xhci->xfer_busy)
            return handled;
        usb_urb_t *ready[XHCI_IRQ_BATCH];
        unsigned n = 0;
        xhci_trb_t event;
        while (n < XHCI_IRQ_BATCH && xhci_next_event(xhci, &event))
            n += xhci_collect_interrupt(xhci, &event, ready + n);
        if (!n)
            return handled;
        handled = 1;
        xhci_run_completions(ready, n, xhci, flags);
    }
}

/* Drain the event ring from the IRQ handler.  A process-context transfer that
 * owns the ring is the only party that can match a TRB to the request that
 * armed it, so in that case the handler acknowledges the pending bit and
 * leaves the TRB for the waiter -- that is what keeps a level-triggered line
 * from re-firing in a tight loop under a bounded busy-wait. */
static int xhci_irq_handler(int irq, void *priv) {
    xhci_controller_t *xhci = (xhci_controller_t *)priv;
    (void)irq;
    if (!xhci)
        return 0;
    uint64_t flags = spin_lock_irqsave(&xhci->lock);
    if (!xhci->running || xhci->xfer_busy || !xhci_irq_pending(xhci)) {
        xhci_ack_event_irq(xhci);
        spin_unlock_irqrestore(&xhci->lock, flags);
        return 0;
    }
    xhci_drain_events(xhci, &flags);
    xhci_ack_event_irq(xhci);
    xhci->irq_events += 1;
    spin_unlock_irqrestore(&xhci->lock, flags);
    return 0;
}

static int xhci_event_matches(const xhci_trb_t *event, uint8_t wanted_type,
                              uint64_t pointer,
                              const xhci_control_trbs_t *control) {
    if (((event->control >> 10) & 0x3fU) != wanted_type)
        return 0;
    uint64_t event_pointer = event->parameter & ~0x0fULL;
    if (event_pointer == pointer)
        return 1;
    /* A terminal error can refer to the Setup or Data TRB. A short
     * packet at Data still has a Status stage to complete. */
    uint8_t cc = (uint8_t)(event->status >> 24);
    return control && cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PACKET &&
           (event_pointer == control->setup ||
            (control->data && event_pointer == control->data));
}

static int xhci_quiesce_failed(xhci_controller_t *xhci)
{
    uint64_t flags = spin_lock_irqsave(&xhci->lock);
    xhci->fatal = 1;
    xhci->running = 0;
    xhci_write32(xhci->runtime + XHCI_INTERRUPTER_OFFSET, 0x00U, 0);
    xhci_write32(xhci->op, XHCI_USBCMD,
                 xhci_read32(xhci->op, XHCI_USBCMD) & ~(XHCI_CMD_RUN | XHCI_CMD_INTE));
    (void)xhci_read32(xhci->op, XHCI_USBCMD);
    /* A software fatal flag alone does not stop DMA into a caller's buffer
     * after the failed transfer returns and that buffer is freed. */
    if (xhci->hcd.hcd_dev) {
        uint16_t command = pci_cfg_read16(xhci->hcd.hcd_dev, 4);
        pci_cfg_write16(xhci->hcd.hcd_dev, 4, command & ~4U);
        (void)pci_cfg_read16(xhci->hcd.hcd_dev, 4);
    }
    spin_unlock_irqrestore(&xhci->lock, flags);
    return xhci_wait32(xhci->op, XHCI_USBSTS, XHCI_STS_HALTED, XHCI_STS_HALTED);
}

static int xhci_wait_event(xhci_controller_t *xhci, uint8_t wanted_type,
                           uint64_t pointer, xhci_trb_t *result,
                           const xhci_control_trbs_t *control) {
    if (xhci->fatal)
        return -ENODEV;
    int any = 0;
    uint16_t dequeue = 0;
    xhci_trb_t unmatched = {0};
    int rc = -ETIMEDOUT;
    uint64_t deadline = xhci->xfer_deadline;
    for (uint32_t i = 0;; i++) {
        if ((i & (XHCI_POLL_SPINS - 1U)) == 0 && timer_get_ticks() >= deadline)
            break;
        /* The controller lock -- and with it local interrupts -- is taken per
         * attempt and released again before the poll relaxes.  It is never held
         * across the wait: that would cost this CPU a timer tick, every other
         * device's interrupt and IPI service for the whole transfer
         * budget, which on a uniprocessor build is the entire machine. */
        uint64_t flags = spin_lock_irqsave(&xhci->lock);
        usb_urb_t *ready[XHCI_IRQ_BATCH];
        unsigned n;
        xhci_trb_t event;
        if (!xhci_next_event(xhci, &event)) {
            spin_unlock_irqrestore(&xhci->lock, flags);
            arch_cpu_relax();
            continue;
        }
        any++;
        /* An interrupt endpoint completing while we wait for a control event:
         * hand it to its owner and keep waiting. */
        n = xhci_collect_interrupt(xhci, &event, ready);
        if (n) {
            /* Runs the callbacks with the lock dropped and returns with it
             * held again. */
            xhci_run_completions(ready, n, xhci, &flags);
            dequeue = xhci->event_dequeue;
            spin_unlock_irqrestore(&xhci->lock, flags);
            continue;
        }
        dequeue = xhci->event_dequeue;
        if (xhci_event_matches(&event, wanted_type, pointer, control)) {
            if (result)
                *result = event;
            uint8_t cc = (uint8_t)(event.status >> 24);
            rc = (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) ? 0 : -cc;
            spin_unlock_irqrestore(&xhci->lock, flags);
            break;
        }
        unmatched = event;
        spin_unlock_irqrestore(&xhci->lock, flags);
    }
    if (rc == -ETIMEDOUT) {
        uint32_t now_sts = xhci_read32(xhci->op, XHCI_USBSTS);
        if (!xhci->fatal_logged) {
            xhci->fatal_logged = 1;
            kerr("[XHCI] FATAL identity: mmio=0x%llx pci_cmd=0x%x "
                 "ports=%u slots=%u irq=%d start_sts=0x%x now_sts=0x%x "
                 "wanted=%u\n",
                 (unsigned long long)xhci->cap, xhci->pci_command,
                 xhci->max_ports, xhci->max_slots, xhci->irq,
                 xhci->start_sts, now_sts, wanted_type);
            kerr("[XHCI] FATAL irq: msi=%u/%u msix=%u/%u intx=%u\n",
                 xhci->msi_present, xhci->msi_enabled,
                 xhci->msix_present, xhci->msix_enabled,
                 xhci->irq_registered);
        }
        kerr("[XHCI] wait_event timeout: wanted=%u trb=%llx seen=%d deq=%u\n",
             wanted_type, (unsigned long long)pointer, any, dequeue);
        if (unmatched.control)
            kerr("[XHCI] last unmatched event: pointer=%llx status=%08x control=%08x\n",
                 (unsigned long long)unmatched.parameter,
                 unmatched.status, unmatched.control);
        kerr("[XHCI] state: cmd=0x%x sts=0x%x crcr=0x%llx "
             "crcr_start=0x%llx cmd_ring=0x%llx dcbaa=0x%llx cfg=0x%x "
             "hcs2=0x%x sp=%u\n",
             xhci_read32(xhci->op, XHCI_USBCMD),
             xhci_read32(xhci->op, XHCI_USBSTS),
             (unsigned long long)xhci_read64(xhci->op, XHCI_CRCR),
             (unsigned long long)xhci->crcr_start,
             (unsigned long long)xhci->command_ring_pa,
             (unsigned long long)xhci_read64(xhci->op, XHCI_DCBAAP),
             xhci_read32(xhci->op, XHCI_CONFIG), xhci->hcs2_value,
             xhci->sp_count);
        if (xhci->sp_count)
            kerr("[XHCI] sp: array=0x%llx dcbaa0=0x%llx first=0x%llx\n",
                 (unsigned long long)va_to_pa(xhci->scratchpad_array),
                 (unsigned long long)xhci->dcbaa[0],
                 (unsigned long long)xhci->scratchpad_array[0]);
        kerr("[XHCI] event: iman=0x%x erstsz=0x%x erstba=0x%llx "
             "erdp=0x%llx cycle=%u\n",
             xhci_read_intr0(xhci),
             xhci_read32(xhci->runtime + XHCI_INTERRUPTER_OFFSET,
                         XHCI_ERSTSZ_OFFSET),
             (unsigned long long)xhci_read64(
                 xhci->runtime + XHCI_INTERRUPTER_OFFSET,
                 XHCI_ERSTBA_OFFSET),
             (unsigned long long)xhci_read64(
                 xhci->runtime + XHCI_INTERRUPTER_OFFSET,
                 XHCI_ERDP_OFFSET),
             xhci->event_cycle);
        kerr("[XHCI] ring: cmd0=%08x/%08x ev0=%08x/%08x\n",
             xhci->command_ring[0].status,
             xhci->command_ring[0].control,
             xhci->event_ring[0].status,
             xhci->event_ring[0].control);
        int halted = xhci_quiesce_failed(xhci);
        kerr("[XHCI] controller quiesced after completion timeout: halt=%d; bus mastering disabled\n",
             halted);
    }
    return rc;
}

/* Ring the command doorbell.  Caller holds xhci->xfer_busy and must release it
 * afterwards; the wait itself runs without xhci->lock held, which is what
 * keeps a bounded hardware wait out of a spinlock. */
static int xhci_command_locked(xhci_controller_t *xhci, uint64_t parameter,
                               uint32_t status, uint32_t control,
                               xhci_trb_t *event) {
    uint64_t flags = spin_lock_irqsave(&xhci->lock);
    uint16_t index = xhci->command_enqueue;
    xhci_trb_t *command = &xhci->command_ring[index];
    command->parameter = parameter;
    command->status = status;
    command->control = control |
        (xhci->command_cycle ? XHCI_TRB_CYCLE : 0U);
    arch_dma_sync_for_device(command, sizeof(*command));
    uint64_t pointer = va_to_pa(command);
    index++;
    if (index == XHCI_RING_TRBS - 1U) {
        xhci_trb_t *link = &xhci->command_ring[index];
        link->parameter = va_to_pa(xhci->command_ring);
        link->status = 0;
        link->control = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_ENT |
                        (xhci->command_cycle ? XHCI_TRB_CYCLE : 0U);
        arch_dma_sync_for_device(link, sizeof(*link));
        xhci->command_cycle ^= 1U;
        index = 0;
    }
    xhci->command_enqueue = index;
    wmb();
    xhci_write32(xhci->doorbell, 0, 0);
    /* Flush the posted PCIe write before polling the event ring.  Without a
     * read-back the doorbell can still be sitting in a bridge write buffer
     * while the driver spins on memory for a completion that was never
     * requested -- QEMU completes it immediately and hides this. */
    (void)xhci_read32(xhci->doorbell, 0);
    spin_unlock_irqrestore(&xhci->lock, flags);
    return xhci_wait_event(xhci, XHCI_TRB_COMMAND_EVENT, pointer, event, NULL);
}

/* Queue the EP0 stages and return the Status TRB address. The caller holds
 * xfer_busy across queueing and the completion wait. */
static uint64_t xhci_control_locked(xhci_controller_t *xhci, uint8_t slot,
                               const usb_setup_packet_t *setup, void *data,
                               xhci_control_trbs_t *control) {
    uint64_t flags = spin_lock_irqsave(&xhci->lock);
    xhci_ring_t *ring = &xhci->ep0_ring[slot];
    xhci_trb_t *first = &ring->trbs[ring->enqueue];
    uint64_t setup_value = 0;
    memcpy(&setup_value, setup, sizeof(*setup));
    uint32_t trt = setup->length ?
        ((setup->request_type & USB_DIR_IN) ? XHCI_TRB_TRT_IN : XHCI_TRB_TRT_OUT) : 0;
    control->setup = xhci_ring_enqueue(ring, setup_value, 8,
                                       XHCI_TRB_TYPE(XHCI_TRB_SETUP) |
                                       XHCI_TRB_IDT | trt, 1);
    if (setup->length) {
        arch_dma_sync_for_device(data, setup->length);
        uint32_t data_control = XHCI_TRB_TYPE(XHCI_TRB_DATA);
        if (setup->request_type & USB_DIR_IN)
            data_control |= XHCI_TRB_DIR_IN | XHCI_TRB_ISP;
        control->data = xhci_ring_enqueue(ring, va_to_pa(data), setup->length,
                                          data_control, 0);
    }
    uint32_t status_control = XHCI_TRB_TYPE(XHCI_TRB_STATUS) | XHCI_TRB_IOC;
    if (!setup->length || !(setup->request_type & USB_DIR_IN))
        status_control |= XHCI_TRB_DIR_IN;
    uint64_t status_pointer = xhci_ring_enqueue(ring, 0, 0, status_control, 0);
    wmb();
    first->control ^= XHCI_TRB_CYCLE;
    arch_dma_sync_for_device(first, sizeof(*first));
    wmb();
    xhci_write32(xhci->doorbell, (uint32_t)slot * 4U, 1U);
    (void)xhci_read32(xhci->doorbell, (uint32_t)slot * 4U);
    spin_unlock_irqrestore(&xhci->lock, flags);
    return status_pointer;
}

static int xhci_control(xhci_controller_t *xhci, uint8_t slot,
                        const usb_setup_packet_t *setup, void *data) {
    if (xhci->fatal)
        return -ENODEV;
    if (!slot || slot > xhci->max_slots)
        return -EINVAL;
    if (setup->length && !data)
        return -EINVAL;
    if (setup->length && setup->length > sizeof(xhci->control_buffer))
        return -EINVAL;
    int r = xhci_xfer_acquire(xhci, XHCI_XFER_HOLD_BUDGET_MS);
    if (r)
        return r;
    /* One bounce buffer per controller: the EP0 data stage has to be a DMA
     * buffer, and xfer_busy is what makes it single-writer.  Copying it here
     * rather than in the caller is what keeps the window between the copy and
     * the transfer from being a second race on the same bytes. */
    void *stage = data;
    if (setup->length && data) {
        stage = xhci->control_buffer;
        if (setup->request_type & USB_DIR_IN)
            memset(stage, 0, setup->length);
        else
            memcpy(stage, data, setup->length);
    }
    xhci_control_trbs_t control = {0};
    xhci_trb_t event = {0};
    uint64_t status_pointer = xhci_control_locked(xhci, slot, setup, stage,
                                                  &control);
    r = xhci_wait_event(xhci, XHCI_TRB_TRANSFER_EVENT, status_pointer, &event,
                        &control);
    if (r)
        kwarn("[XHCI] EP0 slot=%u request=%02x/%02x value=%04x length=%u "
              "failed=%d cc=%u trb=%llx (setup=%llx data=%llx status=%llx)\n",
              slot, setup->request_type, setup->request, setup->value,
              setup->length, r, event.status >> 24,
              (unsigned long long)event.parameter,
              (unsigned long long)control.setup,
              (unsigned long long)control.data,
              (unsigned long long)status_pointer);
    if (r == 0 && setup->length && (setup->request_type & USB_DIR_IN)) {
        arch_dma_sync_for_cpu(stage, setup->length);
        memcpy(data, stage, setup->length);
    }
    xhci_xfer_release(xhci);
    return r;
}

/* ------------------------------------------------------------------ */
/* Context helpers                                                     */
/* ------------------------------------------------------------------ */

static uint8_t xhci_default_mps(uint8_t speed) {
    if (speed >= 4)
        return 9;               /* SuperSpeed: 512 bytes = 2^9 */
    if (speed == USB_SPEED_HIGH)
        return 64;              /* High-speed */
    if (speed == USB_SPEED_FULL)
        return 64;              /* Full-speed: Linux's USB core starts with
                                 * a 64-byte EP0 guess; bMaxPacketSize0 is
                                 * read from the first descriptor later. */
    return 8;                   /* Low-speed */
}

static uint16_t xhci_mps_value(uint8_t speed, uint8_t descriptor_value) {
    if (speed >= 4)
        return (uint16_t)(1U << descriptor_value);
    return descriptor_value;
}

static uint8_t xhci_interval(uint8_t speed, uint8_t usb_interval) {
    if (!usb_interval)
        return 0;
    if (speed >= 3)
        return usb_interval > 16 ? 15 : (uint8_t)(usb_interval - 1U);
    uint32_t microframes = (uint32_t)usb_interval * 8U;
    uint8_t interval = 0;
    while ((1U << interval) < microframes && interval < 15)
        interval++;
    return interval;
}

static int xhci_slot_path(xhci_controller_t *xhci, unsigned port,
                          uint8_t speed, uint8_t hub_slot,
                          xhci_slot_path_t *path)
{
    if (!port || port > USB_HUB_MAX_PORTS || hub_slot > xhci->max_slots)
        return -EINVAL;
    *path = (xhci_slot_path_t){ .speed = speed };
    if (!hub_slot) {
        if (port > xhci->max_ports)
            return -EINVAL;
        path->root_port = (uint8_t)port;
        return 0;
    }
    const xhci_slot_path_t *parent = &xhci->slot_path[hub_slot];
    if (!parent->root_port || parent->depth >= XHCI_MAX_ROUTE_DEPTH)
        return -EINVAL;
    path->root_port = parent->root_port;
    path->depth = parent->depth + 1U;
    /* Section 4.19.7: port 15 and higher share the route nibble 0xf. */
    uint32_t route_port = port > 15U ? 15U : port;
    path->route = parent->route | (route_port << (4U * parent->depth));
    if (speed < USB_SPEED_HIGH) {
        if (parent->speed == USB_SPEED_HIGH) {
            path->tt_slot = hub_slot;
            path->tt_port = (uint8_t)port;
        } else {
            path->tt_slot = parent->tt_slot;
            path->tt_port = parent->tt_port;
        }
    }
    return 0;
}

static void xhci_fill_slot_context(xhci_controller_t *xhci,
                                   const xhci_slot_path_t *path) {
    uint32_t *slot = xhci_input_context(xhci, 1);
    slot[0] = path->route | ((uint32_t)path->speed << 20) | (1U << 27);
    slot[1] = (uint32_t)path->root_port << 16;
    slot[2] = path->tt_slot | ((uint32_t)path->tt_port << 8);
    /* The controller assigns the USB address and reports it in the output
     * slot context. Device State stays zero in the Address Device input. */
}

static void xhci_fill_ep_context(xhci_controller_t *xhci, uint8_t dci,
                                 xhci_ring_t *ring, uint8_t ep_type,
                                 uint16_t max_packet, uint8_t interval) {
    uint32_t *ep = xhci_input_context(xhci, (unsigned)dci + 1U);
    ep[0] = (uint32_t)interval << 16;
    /* Isochronous endpoints do not retry transactions (CERR is zero). */
    uint32_t errors = (ep_type == 1U || ep_type == 5U) ? 0U : 3U;
    ep[1] = (errors << 1) | ((uint32_t)ep_type << 3) |
            ((uint32_t)max_packet << 16);
    /* Ring Dequeue Pointer: the next TRB this ring will produce, carrying the
     * cycle bit that TRB is written with.  Hard-coding trbs[0] with cycle 1
     * only ever matched the ring's first lap -- the controller compares the
     * cycle bit of the TRB it reads against the one here and stops fetching
     * once the ring has wrapped, which is why the pointer has to track
     * ring->enqueue/ring->cycle. */
    uint64_t dequeue = va_to_pa(&ring->trbs[ring->enqueue]) |
                       (ring->cycle ? XHCI_TRB_CYCLE : 0U);
    ep[2] = (uint32_t)dequeue;
    ep[3] = (uint32_t)(dequeue >> 32);
    /* Dword 4 holds Average TRB Length in bits 15:0. Control endpoints
     * require 8. Max ESIT Payload Lo is in bits 31:16 for periodic endpoints. */
    ep[4] = ep_type == 4U ? 8U : max_packet;
    if (ep_type == 1U || ep_type == 3U || ep_type == 5U || ep_type == 7U)
        ep[4] |= (uint32_t)max_packet << 16;
}

/* bmAttributes' transfer type and the endpoint context's EP Type field do not
 * use the same encoding.  bmAttributes says "interrupt" = 3 and says nothing
 * about direction; the endpoint context spells the same endpoint
 * "interrupt IN" = 7 (xHCI 1.2 table 6-9). Values 1..3 are OUT, 4 is control,
 * and 5..7 are IN. Programming bulk IN as 5 makes it isochronous, so the
 * first BOT data/CSW transfer cannot complete as a bulk transaction. */
static uint8_t xhci_ep_type(uint8_t usb_type, uint8_t addr) {
    switch (usb_type) {
    case USB_XFER_CONTROL:
        return 4U;
    case USB_XFER_ISOC:
        return (addr & USB_DIR_IN) ? 5U : 1U;
    case USB_XFER_BULK:
        return (addr & USB_DIR_IN) ? 6U : 2U;
    case USB_XFER_INTERRUPT:
        return (addr & USB_DIR_IN) ? 7U : 3U;
    default:
        return 0U;
    }
}

static void xhci_slot_add_context(uint32_t *slot, uint8_t dci)
{
    uint32_t entries = (slot[0] >> 27) & 0x1fU;
    if (entries < dci)
        entries = dci;
    slot[0] = (slot[0] & ~(0x1fU << 27)) | (entries << 27);
}

/* ------------------------------------------------------------------ */
/* usb_hcd_ops                                                         */
/* ------------------------------------------------------------------ */

static int xhci_op_port_connected(usb_hcd_t *hcd, unsigned port)
{
    xhci_controller_t *xhci = hcd->priv;
    if (xhci->fatal || port < 1 || port > xhci->max_ports)
        return 0;
    uint32_t portsc = xhci_read32(xhci->op,
                                  XHCI_PORTSC_BASE +
                                  (port - 1U) * XHCI_PORTSC_STRIDE);
    return (portsc & XHCI_PORT_CCS) != 0;
}

static int xhci_op_reset_port(usb_hcd_t *hcd, unsigned port, uint8_t *speed) {
    xhci_controller_t *xhci = hcd->priv;
    if (xhci->fatal || port < 1 || port > xhci->max_ports)
        return -ENODEV;
    uint32_t offset = XHCI_PORTSC_BASE + (port - 1U) * XHCI_PORTSC_STRIDE;
    uint32_t value = xhci_read32(xhci->op, offset);
    if (!(value & XHCI_PORT_POWER) && xhci->port_power_control) {
        xhci_write32(xhci->op, offset,
                     (value & XHCI_PORT_PRESERVE) | XHCI_PORT_POWER);
        mdelay(20);
        value = xhci_read32(xhci->op, offset);
    }
    if (!(value & XHCI_PORT_CCS))
        return -ENODEV;
    uint32_t write = value & (XHCI_PORT_PRESERVE | XHCI_PORT_CHANGE_BITS);
    if (XHCI_PORT_SPEED(value) >= 4)
        write |= XHCI_PORT_WARM_RESET;
    else
        write |= XHCI_PORT_RESET;
    xhci_write32(xhci->op, offset, write);
    for (uint32_t i = 0; i < XHCI_WAIT_LOOPS; i++) {
        value = xhci_read32(xhci->op, offset);
        if (!(value & XHCI_PORT_CCS))
            return -ENODEV;
        if (!(value & (XHCI_PORT_RESET | XHCI_PORT_WARM_RESET)) &&
            (value & XHCI_PORT_PED)) {
            /* PED is RW1CS. A write of one disables the port, so write
             * only the persistent state and the observed change flags. */
            xhci_write32(xhci->op, offset,
                         value & (XHCI_PORT_PRESERVE | XHCI_PORT_CHANGE_BITS));
            value = xhci_read32(xhci->op, offset);
            if ((value & (XHCI_PORT_CCS | XHCI_PORT_PED)) !=
                (XHCI_PORT_CCS | XHCI_PORT_PED))
                return -ENODEV;
            *speed = (uint8_t)XHCI_PORT_SPEED(value);
            kinfo("[XHCI] port %u reset: speed=%u portsc=0x%x\n",
                  port, *speed, value);
            return 0;
        }
        arch_cpu_relax();
    }
    return -ETIMEDOUT;
}

static int xhci_op_alloc_slot(usb_hcd_t *hcd, unsigned port, uint8_t speed,
                              uint8_t hub_slot, uint8_t address,
                              usb_slot_t *out) {
    xhci_controller_t *xhci = hcd->priv;
    if (xhci->fatal)
        return -ENODEV;
    xhci_trb_t event;
    /* Both commands below write the shared input context, so the whole
     * slot setup runs as one serialised transfer. */
    int result = xhci_xfer_acquire(xhci, XHCI_XFER_HOLD_BUDGET_MS);
    if (result)
        return result;
    xhci_slot_path_t path;
    result = xhci_slot_path(xhci, port, speed, hub_slot, &path);
    if (result)
        goto out;
    result = xhci_command_locked(xhci, 0, 0,
                                 XHCI_TRB_TYPE(XHCI_TRB_ENABLE_SLOT), &event);
    if (result)
        goto out;
    uint8_t slot = (uint8_t)(event.control >> 24);
    if (!slot || slot > xhci->max_slots) {
        result = -EIO;
        goto out;
    }

    memset(xhci->input_context, 0, sizeof(xhci->input_context));
    uint32_t *control = xhci_input_context(xhci, 0);
    control[1] = (1U << 0) | (1U << 1);
    xhci_fill_slot_context(xhci, &path);
    xhci_ring_init(&xhci->ep0_ring[slot]);
    uint16_t mps = xhci_mps_value(speed, xhci_default_mps(speed));
    xhci_fill_ep_context(xhci, 1, &xhci->ep0_ring[slot], 4, mps, 0);
    memset(xhci->output_context[slot], 0, sizeof(xhci->output_context[slot]));
    xhci->dcbaa[slot] = va_to_pa(xhci->output_context[slot]);
    arch_dma_sync_for_device(xhci->output_context[slot],
                             sizeof(xhci->output_context[slot]));
    arch_dma_sync_for_device(xhci->input_context, sizeof(xhci->input_context));
    arch_dma_sync_for_device(xhci->dcbaa, sizeof(xhci->dcbaa));
    xhci_trb_t completion = {0};
    result = xhci_command_locked(xhci, va_to_pa(xhci->input_context), 0,
                                 XHCI_TRB_TYPE(XHCI_TRB_ADDRESS_DEVICE) |
                                 ((uint32_t)slot << 24), &completion);
    out->hcd = slot;
    out->address = 0;
    if (result) {
        unsigned cc = completion.status >> 24;
        kerr("[XHCI] slot %u ADDRESS_DEVICE(%u) failed: %d cc=%u%s "
             "port=%u speed=%u mps=%u ctx=%u\n",
             slot, address, result, cc,
             cc == XHCI_CC_PARAMETER_ERROR ? " (Parameter Error)" : "",
             port, speed, mps, xhci->context_size);
        if (cc == XHCI_CC_PARAMETER_ERROR) {
            const uint32_t *in_slot = xhci_input_context(xhci, 1);
            const uint32_t *ep0 = xhci_input_context(xhci, 2);
            kerr("[XHCI] Address Device input: add=0x%x "
                 "slot=%08x/%08x/%08x/%08x ep0=%08x/%08x/%08x/%08x/%08x\n",
                 control[1], in_slot[0], in_slot[1], in_slot[2], in_slot[3],
                 ep0[0], ep0[1], ep0[2], ep0[3], ep0[4]);
        }
        if (!xhci->fatal)
            (void)xhci_command_locked(xhci, 0, 0,
                                      XHCI_TRB_TYPE(XHCI_TRB_DISABLE_SLOT) |
                                      ((uint32_t)slot << 24), NULL);
        goto out;
    }
    arch_dma_sync_for_cpu(xhci->output_context[slot],
                          sizeof(xhci->output_context[slot]));
    /* Linux reads the address the controller assigned from the output slot
     * context (dev_state bits 7:0) and uses that; it does not force the USB
     * core's reservation and does not send a second SET_ADDRESS.  The core
     * frees its reservation when slot.address differs from it. */
    uint32_t *out_slot = xhci_output_context(xhci, slot, 0);
    out->address = (uint8_t)(out_slot[3] & 0xffU);
    xhci->slot_path[slot] = path;
    /* USB 2.0 reset-recovery time after the Address Device command. */
    mdelay(2);
out:
    xhci_xfer_release(xhci);
    return result;
}

static int xhci_op_configure_hub(usb_hcd_t *hcd, usb_device_t *dev,
                                 uint8_t ports, uint8_t tt_think_time)
{
    xhci_controller_t *xhci = hcd->priv;
    if (xhci->fatal || !dev->slot || dev->slot > xhci->max_slots)
        return -ENODEV;
    int result = xhci_xfer_acquire(xhci, XHCI_XFER_HOLD_BUDGET_MS);
    if (result)
        return result;
    memset(xhci->input_context, 0, sizeof(xhci->input_context));
    arch_dma_sync_for_cpu(xhci->output_context[dev->slot],
                         sizeof(xhci->output_context[dev->slot]));
    uint32_t *control = xhci_input_context(xhci, 0);
    uint32_t *slot = xhci_input_context(xhci, 1);
    memcpy(slot, xhci_output_context(xhci, dev->slot, 0), xhci->context_size);
    control[1] = 1U;             /* add Slot Context only */
    slot[0] |= 1U << 26;         /* Hub, single TT on the default altsetting */
    slot[3] = 0;
    unsigned command = XHCI_TRB_EVALUATE_CONTEXT;
    if (xhci->hci_version > 0x95) {
        slot[1] = (slot[1] & 0x00ffffffU) | ((uint32_t)ports << 24);
        if (dev->speed == USB_SPEED_HIGH)
            slot[2] |= (uint32_t)tt_think_time << 16;
        command = XHCI_TRB_CONFIGURE_ENDPOINT;
    }
    arch_dma_sync_for_device(xhci->input_context, sizeof(xhci->input_context));
    result = xhci_command_locked(xhci, va_to_pa(xhci->input_context), 0,
                                 XHCI_TRB_TYPE(command) |
                                 ((uint32_t)dev->slot << 24), NULL);
    xhci_xfer_release(xhci);
    return result;
}

static int xhci_op_update_ep0_mps(usb_hcd_t *hcd, uint8_t slot,
                                  uint16_t max_packet) {
    xhci_controller_t *xhci = hcd->priv;
    int r = xhci_xfer_acquire(xhci, XHCI_XFER_HOLD_BUDGET_MS);
    if (r)
        return r;
    memset(xhci->input_context, 0, sizeof(xhci->input_context));
    arch_dma_sync_for_cpu(xhci->output_context[slot],
                          sizeof(xhci->output_context[slot]));
    uint32_t *control = xhci_input_context(xhci, 0);
    control[1] = 1U << 1;
    uint32_t *out_ep0 = xhci_output_context(xhci, slot, 1);
    uint32_t *in_ep0 = xhci_input_context(xhci, 2);
    memcpy(in_ep0, out_ep0, xhci->context_size);
    in_ep0[1] = (in_ep0[1] & 0x0000ffffU) | ((uint32_t)max_packet << 16);
    arch_dma_sync_for_device(xhci->input_context, sizeof(xhci->input_context));
    r = xhci_command_locked(xhci, va_to_pa(xhci->input_context), 0,
                            XHCI_TRB_TYPE(XHCI_TRB_EVALUATE_CONTEXT) |
                            ((uint32_t)slot << 24), NULL);
    xhci_xfer_release(xhci);
    return r;
}

static int xhci_op_control(usb_hcd_t *hcd, uint8_t slot,
                           const usb_setup_packet_t *setup, void *data) {
    xhci_controller_t *xhci = hcd->priv;
    return xhci_control(xhci, slot, setup, data);
}

static int xhci_op_get_descriptor(usb_hcd_t *hcd, uint8_t slot, uint8_t type,
                                  uint16_t len, void *buf) {
    xhci_controller_t *xhci = hcd->priv;
    if (len > sizeof(xhci->control_buffer))
        return -EINVAL;
    usb_setup_packet_t setup = {
        .request_type = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .request = USB_REQ_GET_DESCRIPTOR,
        .value = (uint16_t)type << 8,
        .index = 0,
        .length = len,
    };
    return xhci_control(xhci, slot, &setup, buf);
}

static int xhci_op_configure_endpoint(usb_hcd_t *hcd, usb_device_t *dev,
                                      uint8_t addr, uint8_t ep_type,
                                      uint16_t max_packet, uint8_t interval) {
    xhci_controller_t *xhci = hcd->priv;
    if (xhci->fatal)
        return -ENODEV;
    uint8_t number = addr & 0x0fU;
    if (!number)
        return -EINVAL;
    uint8_t dci = (uint8_t)((addr & USB_DIR_IN) ? number * 2U + 1U : number * 2U);
    if (dci >= 32 || xhci->ep_count >= XHCI_MAX_EPS)
        return -EINVAL;

xhci_ep_t *ep = NULL;
    int alloc = xhci_alloc_ep(hcd->hcd_dev, &ep);
    if (alloc)
        return alloc;
    xhci_ring_init(&ep->ring);
    ep->slot = dev->slot;
    ep->dci = dci;
    ep->addr = addr;

    /* The input context is per controller, so the whole endpoint setup runs
     * as one serialised transfer. */
    int result = xhci_xfer_acquire(xhci, XHCI_XFER_HOLD_BUDGET_MS);
    if (result) {
        kfree(ep->alloc_ptr);
        return result;
    }
    memset(xhci->input_context, 0, sizeof(xhci->input_context));
    arch_dma_sync_for_cpu(xhci->output_context[dev->slot],
                          sizeof(xhci->output_context[dev->slot]));
    uint32_t *control = xhci_input_context(xhci, 0);
    uint32_t *out_slot = xhci_output_context(xhci, dev->slot, 0);
    uint32_t *in_slot = xhci_input_context(xhci, 1);
    memcpy(in_slot, out_slot, xhci->context_size);
    control[1] = 1U;
    control[1] |= 1U << dci;
    xhci_fill_ep_context(xhci, dci, &ep->ring, xhci_ep_type(ep_type, addr),
                         max_packet, xhci_interval(dev->speed, interval));
    /* Adding a lower-numbered endpoint must not hide existing higher ones:
     * BOT commonly configures OUT EP2 (DCI4) before IN EP1 (DCI3). */
    xhci_slot_add_context(in_slot, dci);
    arch_dma_sync_for_device(xhci->input_context, sizeof(xhci->input_context));
    result = xhci_command_locked(xhci, va_to_pa(xhci->input_context), 0,
                                 XHCI_TRB_TYPE(XHCI_TRB_CONFIGURE_ENDPOINT) |
                                 ((uint32_t)dev->slot << 24), NULL);
    if (result) {
        xhci_xfer_release(xhci);
        kfree(ep->alloc_ptr);
        return result;
    }
    /* Publish the endpoint before dropping xfer_busy, so a completion that
     * arrives the moment the transfer ends already finds it. */
    uint64_t flags = spin_lock_irqsave(&xhci->lock);
    ep->next = xhci->eps;
    xhci->eps = ep;
    xhci->ep_count++;
    spin_unlock_irqrestore(&xhci->lock, flags);
    xhci_xfer_release(xhci);
    return 0;
}

static int xhci_op_submit_interrupt(usb_hcd_t *hcd, usb_urb_t *urb) {
    xhci_controller_t *xhci = hcd->priv;
    if (xhci->fatal)
        return -ENODEV;
    if (!urb || !urb->ep)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&xhci->lock);
    uint8_t addr = urb->ep->addr;
    uint8_t number = addr & 0x0fU;
    uint8_t dci = (uint8_t)((addr & USB_DIR_IN) ? number * 2U + 1U : number * 2U);
    xhci_ep_t *ep = xhci_find_ep(xhci, urb->dev->slot, dci);
    if (!ep) {
        spin_unlock_irqrestore(&xhci->lock, flags);
        return -ENODEV;
    }
    if (ep->pending) {
        spin_unlock_irqrestore(&xhci->lock, flags);
        return -EBUSY;
    }
    memset(urb->buf, 0, urb->len);
    arch_dma_sync_for_device(urb->buf, urb->len);
    /* A Normal TRB gets its direction from the endpoint context. Bit 16 is
     * reserved here; DIR belongs to a control Data Stage TRB only. */
    xhci_ring_enqueue(&ep->ring, va_to_pa(urb->buf), (uint32_t)urb->len,
                      XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_ISP |
                      XHCI_TRB_IOC, 0);
    ep->pending = urb;
    wmb();
    xhci_write32(xhci->doorbell, (uint32_t)urb->dev->slot * 4U, dci);
    (void)xhci_read32(xhci->doorbell, (uint32_t)urb->dev->slot * 4U);
    spin_unlock_irqrestore(&xhci->lock, flags);
    return 0;
}

/* Bulk transfer: arm a NORMAL TRB on the endpoint ring and wait for its
 * transfer event.  Synchronous (like the control path) because BOT is a
 * strictly serial CBW → data → CSW protocol.  We deliberately do NOT arm
 * ep->pending: xhci_wait_event consumes pending URBs' events and continues,
 * which would starve this synchronous wait.  The caller receives the result
 * directly from the return value. */
static int xhci_op_submit_bulk(usb_hcd_t *hcd, usb_urb_t *urb) {
    xhci_controller_t *xhci = hcd->priv;
    if (xhci->fatal)
        return -ENODEV;
    if (!urb || !urb->ep)
        return -EINVAL;
    /* Serialised like every other synchronous transfer: one owner of the event
     * ring, and it stays released while the bounded wait runs. */
    int acquired = xhci_xfer_acquire(xhci, XHCI_BULK_HOLD_BUDGET_MS);
    if (acquired)
        return acquired;
    int result;
    uint64_t flags = spin_lock_irqsave(&xhci->lock);
    uint8_t addr = urb->ep->addr;
    uint8_t number = addr & 0x0fU;
    uint8_t dci = (uint8_t)((addr & USB_DIR_IN) ? number * 2U + 1U : number * 2U);
    xhci_ep_t *ep = xhci_find_ep(xhci, urb->dev->slot, dci);
    if (!ep) {
        spin_unlock_irqrestore(&xhci->lock, flags);
        result = -ENODEV;
        goto out;
    }
    if (ep->pending) {
        spin_unlock_irqrestore(&xhci->lock, flags);
        result = -EBUSY;
        goto out;
    }

    arch_dma_sync_for_device(urb->buf, urb->len);
    /* Direction is already encoded in this bulk endpoint's EP Type. */
    uint64_t pointer = xhci_ring_enqueue(&ep->ring, va_to_pa(urb->buf),
                                         (uint32_t)urb->len,
                                         XHCI_TRB_TYPE(XHCI_TRB_NORMAL) |
                                         XHCI_TRB_ISP | XHCI_TRB_IOC, 0);
    wmb();
    xhci_write32(xhci->doorbell, (uint32_t)urb->dev->slot * 4U, dci);
    (void)xhci_read32(xhci->doorbell, (uint32_t)urb->dev->slot * 4U);
    spin_unlock_irqrestore(&xhci->lock, flags);

    result = xhci_wait_event(xhci, XHCI_TRB_TRANSFER_EVENT, pointer, NULL, NULL);
    if (result == 0)
        arch_dma_sync_for_cpu(urb->buf, urb->len);
    else
        kwarn("[XHCI] bulk slot=%u endpoint=%02x length=%u failed=%d trb=%llx\n",
              urb->dev->slot, addr, (unsigned)urb->len, result,
              (unsigned long long)pointer);
out:
    xhci_xfer_release(xhci);
    return result;
}

/* Tear a slot down on removal.
 *
 * The controller has to stop using the slot before its rings are freed, or it
 * can keep DMAing through a request that is already in flight while the memory
 * behind it is returned to the allocator.  Two steps do that: wait for the
 * synchronous-transfer owner (xfer_busy), then issue Disable Slot, which halts
 * every endpoint of the slot.  Only after the command completes may the DCBAA
 * entry be cleared and the endpoint allocations freed.
 *
 * xfer_busy is held for the whole wait of a transfer, not just its submission,
 * so a transfer that is still waiting for a completion event has to finish or
 * time out first.  The acquire is retried a few times because the holder's own
 * budget is 500 ms and a removal thread must not permanently give up after one
 * of those; if it never clears, the rings stay allocated (a leak, but not a
 * use-after-free) and the slot is reported unreleased. */
static int xhci_op_abort_slot(usb_hcd_t *hcd, uint8_t slot) {
    xhci_controller_t *xhci = hcd->priv;
    if (!slot || slot > xhci->max_slots)
        return -EINVAL;

    int held = -EBUSY;
    for (int attempt = 0; attempt < 8 && held < 0; attempt++) {
        held = xhci_xfer_acquire(xhci, XHCI_XFER_HOLD_BUDGET_MS);
        if (held < 0)
            mdelay(10);
    }
    if (held < 0) {
        kerr("[XHCI] slot %u: transfer owner did not clear; context left allocated\n",
             slot);
        return held;
    }

    int r = xhci_command_locked(xhci, 0, 0,
                                XHCI_TRB_TYPE(XHCI_TRB_DISABLE_SLOT) |
                                ((uint32_t)slot << 24), NULL);
    uint64_t flags = spin_lock_irqsave(&xhci->lock);
    /* A Disable Slot that failed leaves the device context in place; clearing
     * DCBAA anyway would let the controller fetch a zeroed context. */
    if (r == 0) {
        xhci->dcbaa[slot] = 0;
        xhci->slot_path[slot] = (xhci_slot_path_t){0};
        arch_dma_sync_for_device(xhci->dcbaa, sizeof(xhci->dcbaa));
    }
    spin_unlock_irqrestore(&xhci->lock, flags);
    xhci_xfer_release(xhci);
    if (r < 0) {
        kerr("[XHCI] slot %u: Disable Slot failed: %d\n", slot, r);
        return r;
    }

    xhci_ep_t *dead = NULL;
    flags = spin_lock_irqsave(&xhci->lock);
    xhci_ep_t **pp = &xhci->eps;
    while (*pp) {
        xhci_ep_t *e = *pp;
        if (e->slot == slot) {
            *pp = e->next;
            xhci->ep_count--;
            /* Unlink under the lock, free outside it: kfree() under a device
             * lock is exactly what the lock contract forbids. */
            e->next = dead;
            dead = e;
        } else {
            pp = &e->next;
        }
    }
    spin_unlock_irqrestore(&xhci->lock, flags);
    while (dead) {
        xhci_ep_t *e = dead;
        dead = e->next;
        kfree(e->alloc_ptr);
    }
    return 0;
}

/* Polling fallback.  With the interrupt line registered this still runs (the
 * core polls for hotplug), but there is nothing left for it to consume: the
 * handler drains the ring first.  It refuses to touch the ring while a
 * synchronous transfer owns it, because the poll would consume the TRB the
 * waiter is matching against. */
static int xhci_op_poll(usb_hcd_t *hcd) {
    xhci_controller_t *xhci = hcd->priv;
    if (xhci->fatal)
        return 0;
    uint64_t flags = spin_lock_irqsave(&xhci->lock);
    if (xhci->xfer_busy) {
        spin_unlock_irqrestore(&xhci->lock, flags);
        return 0;
    }
    int handled = xhci_drain_events(xhci, &flags);
    xhci->poll_events += (uint64_t)handled;
    xhci_ack_event_irq(xhci);
    uint64_t irq_events = xhci->irq_events;
    uint64_t poll_events = xhci->poll_events;
    /* Announce each completion path once. Per-report logging can exhaust a
     * physical serial console's bandwidth while a USB mouse is moving. */
    if ((irq_events && !xhci->reported_irq_events) ||
        (poll_events && !xhci->reported_poll_events)) {
        xhci->reported_irq_events = irq_events;
        xhci->reported_poll_events = poll_events;
        spin_unlock_irqrestore(&xhci->lock, flags);
        /* Reported from process context, never from the handler itself. */
        kinfo("[XHCI] completions: irq=%llu poll=%llu\n",
              (unsigned long long)irq_events,
              (unsigned long long)poll_events);
        return handled;
    }
    spin_unlock_irqrestore(&xhci->lock, flags);
    return handled;
}

/* xHCI 1.2 sections 7.1/7.1.1.  Legacy Support is optional, but an owner
 * which does advertise it must release the controller before we reset it.
 * Bound the entire extended-capability chain by the actual PCI BAR. */
static int xhci_take_ownership(uintptr_t cap_base, size_t bar_bytes)
{
    uint32_t hcc = xhci_read32(cap_base, 0x10U);
    uint32_t offset = (hcc >> 16) * 4U;
    for (unsigned limit = 0; offset && limit < 64; limit++) {
        if (offset < 0x20U || offset > bar_bytes || bar_bytes - offset < 4U)
            return -EINVAL;
        uint32_t cap = xhci_read32(cap_base, offset);
        uint8_t id = (uint8_t)cap;
        uint32_t next = ((cap >> 8) & 0xffU) * 4U;
        if (id == 1) {
            if (bar_bytes - offset < 8U)
                return -EINVAL;
            xhci_write32(cap_base, offset, cap | (1U << 24));
            unsigned ms;
            for (ms = 0; ms < XHCI_FIRMWARE_TIMEOUT_MS; ms++) {
                if (!(xhci_read32(cap_base, offset) & (1U << 16)))
                    break;
                mdelay(1);
            }
            if (ms == XHCI_FIRMWARE_TIMEOUT_MS)
                return -ETIMEDOUT;
            /* USBLEGCTLSTS: disable the defined SMI enables, acknowledge its
             * three RW1C event bits, retain the remaining reserved fields.
             * Layout matches Linux's xhci-ext-caps.h/pci-quirks.c. */
            uint32_t ctl = xhci_read32(cap_base, offset + 4U);
            ctl &= (0x7U << 1) | (0xffU << 5) | (0x7U << 17);
            ctl |= 0x7U << 29;
            xhci_write32(cap_base, offset + 4U, ctl);
            return 0;
        }
        offset = next ? offset + next : 0;
    }
    return offset ? -EINVAL : 0;
}

/* A controller reset clears PORTSC, including PP.  Firmware that booted this
 * very USB stick had the ports powered; after our reset a laptop xHC may leave
 * PP=0, so no device is ever seen again.  Linux's xhci_set_port_power() does
 * the same when HCCPARAMS1.PPC says the controller owns port power. */
static void xhci_power_ports(xhci_controller_t *xhci) {
    if (!xhci->port_power_control)
        return;
    unsigned powered = 0;
    for (unsigned port = 1; port <= xhci->max_ports; port++) {
        uint32_t offset = XHCI_PORTSC_BASE +
                          (port - 1U) * XHCI_PORTSC_STRIDE;
        uint32_t value = xhci_read32(xhci->op, offset);
        if (value & XHCI_PORT_POWER)
            continue;
        xhci_write32(xhci->op, offset,
                     (value & XHCI_PORT_PRESERVE) | XHCI_PORT_POWER);
        powered++;
    }
    if (powered) {
        /* USB2 root ports need their power-good delay before CCS is valid. */
        mdelay(20);
        kinfo("[XHCI] powered %u root port(s)\n", powered);
    }
}

/* Program CRCR with one 64-bit MMIO store.  Do not use a readback to choose
 * a fallback: several controllers return the command-ring pointer field as
 * zero on read even though the write succeeded (Linux documents this for the
 * abort path).  Preserve the low six bits exactly as Linux does, then set the
 * ring pointer and cycle state; crcr_start is kept for the timeout
 * diagnostic. */
static int xhci_program_crcr(xhci_controller_t *xhci, uint64_t ring_pa) {
    if (ring_pa & 0x3fU) {
        kerr("[XHCI] command ring is not 64-byte aligned: 0x%llx\n",
             (unsigned long long)ring_pa);
        return -EINVAL;
    }
    uint64_t current = xhci_read64(xhci->op, XHCI_CRCR);
    uint64_t value = (current & 0x3fU) | (ring_pa & ~0x3fULL) | 1U;
    /* Write both dwords: QEMU and at least some hardware only latch the
     * command ring dequeue pointer on the complete 64-bit sequence. */
    xhci_write64(xhci->op, XHCI_CRCR, value);
    xhci->command_ring_pa = ring_pa;
    xhci->crcr_start = xhci_read64(xhci->op, XHCI_CRCR);
    if ((xhci->crcr_start & ~0x3fULL) == 0)
        kinfo("[XHCI] CRCR readback=0 after writing 0x%llx "
              "(readback quirk; the command ring still completed)\n", value);
    return 0;
}

static int xhci_op_start(usb_hcd_t *hcd) {
    xhci_controller_t *xhci = hcd->priv;
    int owner = xhci_take_ownership(xhci->cap, xhci->mmio_size);
    if (owner)
        return owner;

    xhci_write32(xhci->op, XHCI_USBCMD,
                 xhci_read32(xhci->op, XHCI_USBCMD) & ~XHCI_CMD_RUN);
    if (xhci_wait32(xhci->op, XHCI_USBSTS, XHCI_STS_HALTED, XHCI_STS_HALTED) != 0) {
        kerr("[XHCI] controller did not halt (USBSTS=0x%x)\n",
             xhci_read32(xhci->op, XHCI_USBSTS));
        return -ETIMEDOUT;
    }
    xhci_write32(xhci->op, XHCI_USBCMD, XHCI_CMD_RESET);
    if (xhci_wait32(xhci->op, XHCI_USBCMD, XHCI_CMD_RESET, 0) != 0) {
        kerr("[XHCI] reset did not complete (USBCMD=0x%x)\n",
             xhci_read32(xhci->op, XHCI_USBCMD));
        return -ETIMEDOUT;
    }
    if (xhci_wait32(xhci->op, XHCI_USBSTS, XHCI_STS_CNR, 0) != 0) {
        kerr("[XHCI] controller not ready after reset (USBSTS=0x%x)\n",
             xhci_read32(xhci->op, XHCI_USBSTS));
        return -ETIMEDOUT;
    }
    uint32_t sticky = xhci_read32(xhci->op, XHCI_USBSTS) &
                      (XHCI_STS_FATAL | XHCI_STS_EINT |
                       XHCI_STS_PCD | XHCI_STS_HSE);
    if (sticky) {
        xhci_write32(xhci->op, XHCI_USBSTS, sticky);
        (void)xhci_read32(xhci->op, XHCI_USBSTS);
        kinfo("[XHCI] cleared sticky USBSTS=0x%x\n", sticky);
    }
    uint32_t page_size = xhci_read32(xhci->op, XHCI_PAGESIZE);
    if (!(page_size & 1U)) {
        kerr("[XHCI] 4 KiB pages unsupported (PAGESIZE=0x%x)\n", page_size);
        return -ENOSYS;
    }

    memset(xhci->dcbaa, 0, sizeof(xhci->dcbaa));
    uint32_t hcs2 = xhci_read32(xhci->cap, 0x08U);
    unsigned scratchpads = (((hcs2 >> 21) & 0x1fU) << 5) |
                           ((hcs2 >> 27) & 0x1fU);
    xhci->hcs2_value = hcs2;
    xhci->sp_count = scratchpads;
    if (scratchpads > ARRAY_SIZE(xhci->scratchpad_array)) {
        kerr("[XHCI] needs %u scratchpad buffers (max %zu)\n",
             scratchpads, ARRAY_SIZE(xhci->scratchpad_array));
        return -ENOSYS;
    }
    if (scratchpads) {
        kinfo("[XHCI] scratchpad buffers=%u\n", scratchpads);
        for (unsigned i = 0; i < scratchpads; i++)
            xhci->scratchpad_array[i] = va_to_pa(xhci->scratchpads[i]);
        xhci->dcbaa[0] = va_to_pa(xhci->scratchpad_array);
        arch_dma_sync_for_device(xhci->scratchpads,
                                 scratchpads * sizeof(xhci->scratchpads[0]));
        arch_dma_sync_for_device(xhci->scratchpad_array,
                                 scratchpads * sizeof(xhci->scratchpad_array[0]));
    }

    memset(xhci->command_ring, 0, sizeof(xhci->command_ring));
    xhci->command_cycle = 1;
    xhci->command_enqueue = 0;
    xhci->command_ring[XHCI_RING_TRBS - 1U].parameter = va_to_pa(xhci->command_ring);
    xhci->command_ring[XHCI_RING_TRBS - 1U].control =
        XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_ENT | XHCI_TRB_CYCLE;
    memset(xhci->event_ring, 0, sizeof(xhci->event_ring));
    xhci->event_cycle = 1;
    xhci->event_dequeue = 0;
    memset(&xhci->erst, 0, sizeof(xhci->erst));
    xhci->erst.address = va_to_pa(xhci->event_ring);
    xhci->erst.size = XHCI_EVENT_TRBS;
    xhci->erst.reserved = 0;
    arch_dma_sync_for_device(xhci->command_ring, sizeof(xhci->command_ring));
    arch_dma_sync_for_device(xhci->event_ring, sizeof(xhci->event_ring));
    arch_dma_sync_for_device(&xhci->erst, sizeof(xhci->erst));
    arch_dma_sync_for_device(xhci->dcbaa, sizeof(xhci->dcbaa));

    /* Match Linux's xhci_mem_init order: set max device slots, publish the
     * DCBAA, then program the command ring.  Linux writes the DCBAA before
     * CRCR; the reverse order was one of the remaining differences and a
     * controller that validates the command ring against a not-yet-published
     * DCBAA can take an HSE before the first command.  Keep the non-slot
     * CONFIG bits, as Linux does. */
    uint32_t config = xhci_read32(xhci->op, XHCI_CONFIG);
    xhci_write32(xhci->op, XHCI_CONFIG,
                 (config & ~0xffU) | xhci->max_slots);
    xhci_write64(xhci->op, XHCI_DCBAAP, va_to_pa(xhci->dcbaa));
    uint64_t cmd_pa = va_to_pa(xhci->command_ring);
    int crcr_rc = xhci_program_crcr(xhci, cmd_pa);
    if (crcr_rc)
        return crcr_rc;
    uint64_t crcr_readback = xhci->crcr_start;
    uintptr_t ir0 = xhci->runtime + XHCI_INTERRUPTER_OFFSET;
    /* INTR0 bit 0 is IP (write-1-to-clear) and bit 1 is IE, per xHCI 1.2
     * 5.5.2.1.  IE is armed only when this controller owns a registered line,
     * and only after the controller is running, so the ring is primed before
     * any interrupt can be taken. */
    xhci_write32(ir0, 0x00U, XHCI_INTR0_IP);
    xhci_write32(ir0, XHCI_ERSTSZ_OFFSET, 1U);
    xhci_write64(ir0, XHCI_ERSTBA_OFFSET, va_to_pa(&xhci->erst));
    xhci_write64(ir0, XHCI_ERDP_OFFSET, va_to_pa(xhci->event_ring));
    /* INTE rides along with the Run bit.  USBCMD is a plain write-what-you-mean
     * register, so writing Run alone cleared Interrupt Enable again and the
     * controller posted events without ever asserting INTx -- which is what
     * left this driver reporting irq=0 poll=1 forever. */
    xhci_write32(xhci->op, XHCI_USBCMD, XHCI_CMD_RUN | XHCI_CMD_INTE);
    if (xhci_wait32(xhci->op, XHCI_USBSTS, XHCI_STS_HALTED, 0) != 0) {
        kerr("[XHCI] controller did not start (USBSTS=0x%x USBCMD=0x%x)\n",
             xhci_read32(xhci->op, XHCI_USBSTS),
             xhci_read32(xhci->op, XHCI_USBCMD));
        return -ETIMEDOUT;
    }

    xhci_power_ports(xhci);
    xhci->running = 1;
    xhci->start_sts = xhci_read32(xhci->op, XHCI_USBSTS);
    if (xhci->irq_registered)
        xhci_write32(ir0, 0x00U, XHCI_INTR0_IE | XHCI_INTR0_IP);
    kinfo("[XHCI] rings: cmd=0x%llx crcr=0x%llx ev=0x%llx erst=0x%llx "
          "dcbaa=0x%llx\n",
          (unsigned long long)cmd_pa, (unsigned long long)crcr_readback,
          (unsigned long long)va_to_pa(xhci->event_ring),
          (unsigned long long)va_to_pa(&xhci->erst),
          (unsigned long long)va_to_pa(xhci->dcbaa));
    kinfo("[XHCI] controller running: slots=%u ports=%u context=%u sts=0x%x "
          "hcs2=0x%x completion=%s\n",
          xhci->max_slots, xhci->max_ports, xhci->context_size,
          xhci_read32(xhci->op, XHCI_USBSTS), xhci_read32(xhci->cap, 0x08U),
          xhci->irq_registered ? "interrupt" : "polling");
    return 0;
}

static const usb_hcd_ops_t xhci_hcd_ops = {
    .start = xhci_op_start,
    .poll = xhci_op_poll,
    .port_connected = xhci_op_port_connected,
    .reset_port = xhci_op_reset_port,
    .alloc_slot = xhci_op_alloc_slot,
    .configure_hub = xhci_op_configure_hub,
    .update_ep0_mps = xhci_op_update_ep0_mps,
    .control = xhci_op_control,
    .get_descriptor = xhci_op_get_descriptor,
    .configure_endpoint = xhci_op_configure_endpoint,
    .submit_interrupt = xhci_op_submit_interrupt,
    .submit_bulk = xhci_op_submit_bulk,
    .abort_slot = xhci_op_abort_slot,
};

/* xHCI DMA structures (rings, contexts, DCBAA) require 64-byte alignment.
 * The scratchpad buffers embedded in the controller additionally require
 * 4 KiB alignment (xHCI 1.2 6.1), and the struct's own alignment is raised to
 * PAGE_SIZE by that member.  kmalloc only guarantees 8/16-byte alignment, so
 * the controller base must be aligned to a page, not just 64 bytes; otherwise
 * HCS2.MaxScratchpadBuffers controllers get unaligned scratchpad pages and can
 * stop executing commands. */
#define XHCI_DMA_ALIGN 64UL
#define XHCI_CTRL_ALIGN PAGE_SIZE

/* ------------------------------------------------------------------ */
/* PCI driver                                                          */
/* ------------------------------------------------------------------ */

/* Every address handed to the controller lies inside one of these blocks, so
 * one range check over the block covers the rings, the contexts, the DCBAA and
 * the scratchpads without having to re-check each pointer at every call site. */
static int xhci_dma_ok(device_t *dev, const void *va, size_t size)
{
    if (dma_range_ok(dev, va_to_pa(va), size))
        return 0;
    kerr("[XHCI] DMA range 0x%lx..0x%lx outside the declared window\n",
         (unsigned long)va_to_pa(va),
         (unsigned long)(va_to_pa(va) + size - 1UL));
    return -EOPNOTSUPP;
}

static int xhci_alloc_controller(device_t *dev, xhci_controller_t **out)
{
    size_t sz = sizeof(xhci_controller_t) + XHCI_CTRL_ALIGN - 1UL;
    void *raw = kmalloc(sz);
    if (!raw)
        return -ENOMEM;
    xhci_controller_t *xhci = (xhci_controller_t *)
        (((uintptr_t)raw + (XHCI_CTRL_ALIGN - 1UL)) &
         ~(XHCI_CTRL_ALIGN - 1UL));
    memset(xhci, 0, sizeof(*xhci));
    xhci->alloc_ptr = raw;
    int ret = xhci_dma_ok(dev, xhci, sizeof(*xhci));
    if (ret) {
        kfree(raw);
        return ret;
    }
    *out = xhci;
    return 0;
}

static int xhci_alloc_ep(device_t *dev, xhci_ep_t **out)
{
    size_t sz = sizeof(xhci_ep_t) + XHCI_CTRL_ALIGN - 1UL;
    void *raw = kcalloc(1, sz);
    if (!raw)
        return -ENOMEM;
    xhci_ep_t *ep = (xhci_ep_t *)
        (((uintptr_t)raw + (XHCI_CTRL_ALIGN - 1UL)) &
         ~(XHCI_CTRL_ALIGN - 1UL));
    ep->alloc_ptr = raw;
    int ret = xhci_dma_ok(dev, ep, sizeof(*ep));
    if (ret) {
        kfree(raw);
        return ret;
    }
    *out = ep;
    return 0;
}

/* ------------------------------------------------------------------ */
/* PCI driver                                                          */
/* ------------------------------------------------------------------ */

/* Narrow the PCI vendor/device match to the xHCI programming interface
 * (base 0x0C, sub 0x03, prog 0x30). */
static int xhci_pci_match(device_t *dev)
{
    uint32_t cc = pci_class_code(dev);
    return (cc >> 8) == 0x0C03U && (cc & 0xFF) == 0x30U;
}

/* a20.xhci.poll=1 forces the event-ring polling fallback on a board whose INTx
 * line the platform does not route.  It is a diagnostic override, not a
 * capability switch: probe already falls back on its own whenever the line is
 * absent or request_irq() fails, so the knob only exists to force that same
 * fallback where registration would otherwise succeed.  Read once at probe, so
 * the ready line always names the mode actually in force.  Same token scan as
 * mm/wx.c, net/control/net_config.c and drivers/net/e1000.c. */
static int xhci_cmdline_poll(void)
{
    const char *cmdline = bootargs_get();
    if (!cmdline)
        return 0;
    const char *p = cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *tok_end = p;
        while (*tok_end && *tok_end != ' ' && *tok_end != '\t')
            tok_end++;
        static const char key[] = "a20.xhci.poll=";
        /* memcmp(), not strncmp(): this file is also #included verbatim by
         * kernel/drvmod/examples/xhci.c, and a loadable module may only call
         * symbols in the kernel's drv_export_table[] -- strncpy/memcmp/strcmp/
         * strlen are exported (kernel/drvmod/framework.c:313-318) but strncmp is
         * not, and the loader rejects the package outright on an unresolved
         * symbol (kernel/drvmod/loader.c:236-243) rather than failing later at
         * run time.  The length check above already guarantees the token holds
         * at least sizeof(key)-1 bytes, so this reads no further than tok_end. */
        if ((size_t)(tok_end - p) > sizeof(key) - 1 &&
            memcmp(p, key, sizeof(key) - 1) == 0) {
            const char *v = p + sizeof(key) - 1;
            int force = (*v == '1' || *v == 'y' || *v == 'Y');
            kinfo("[XHCI] a20.xhci.poll=%s -> %s completions\n", v,
                  force ? "forced polling" : "interrupt");
            return force;
        }
        p = tok_end;
    }
    return 0;
}

static int xhci_configure_dma_mask(device_t *dev, uint32_t hcc1)
{
    /* HCCPARAMS1.AC64 is readable before allocation.  A 64-bit TRB field
     * alone does not mean that the host implements 64-bit DMA addresses. */
    return dma_set_mask(dev, (hcc1 & 1U) ? DMA_MASK_64BIT : DMA_MASK_32BIT);
}

/* Bay Trail's switchable USB2 wires may still belong to its EHCI companion
 * after firmware exit.  Follow the firmware routing masks; do not touch the
 * PHY, GPIOs, port-disable fuses or routing policy of another Intel chipset.
 * Linux v6.17 pci-quirks.c and coreboot 4.22 baytrail/xhci.c document this
 * USB3-before-USB2 ordering.  Call only after the new xHC is running. */
static int xhci_baytrail_route_ports(device_t *dev)
{
    if (pci_device_id(dev) != 0x80860f35U)
        return 0;
    uint32_t usb3_mask = pci_cfg_read32(dev, 0xdcU);
    uint32_t usb2_mask = pci_cfg_read32(dev, 0xd4U);
    if (usb3_mask & ~1U || usb2_mask & ~0xfU)
        return -EIO;
    uint32_t usb3 = pci_cfg_read32(dev, 0xd8U);
    uint32_t usb2 = pci_cfg_read32(dev, 0xd0U);
    if (usb3 == 0xffffffffU || usb2 == 0xffffffffU)
        return -EIO;
    if ((usb3 & usb3_mask) != usb3_mask)
        pci_cfg_write32(dev, 0xd8U, usb3 | usb3_mask);
    if ((usb2 & usb2_mask) != usb2_mask)
        pci_cfg_write32(dev, 0xd0U, usb2 | usb2_mask);
    if ((pci_cfg_read32(dev, 0xd8U) & usb3_mask) != usb3_mask ||
        (pci_cfg_read32(dev, 0xd0U) & usb2_mask) != usb2_mask)
        return -EIO;
    return 0;
}

static int xhci_probe(device_t *dev)
{
    if (pci_enable_and_assign_bars(dev) != 0)
        return -ENODEV;
    resource_t *bar = pci_get_bar_resource(dev, 0);
    if (!bar || bar->end < bar->start || bar->end - bar->start + 1U < 0x1000U)
        return -ENODEV;

    uintptr_t cap = (uintptr_t)pci_map_bar(dev, 0);
    if (!cap)
        return -EIO;
    uint32_t hcc1 = xhci_read32(cap, 0x10U);
    int mask = xhci_configure_dma_mask(dev, hcc1);
    if (mask < 0)
        return mask;

    xhci_controller_t *xhci = NULL;
    int alloc = xhci_alloc_controller(dev, &xhci);
    if (alloc)
        return alloc;
    spin_init(&xhci->lock);
    xhci->cap = cap;
    xhci->mmio_size = (size_t)(bar->end - bar->start + 1U);
    uint8_t cap_length = readb((const volatile void *)xhci->cap);
    uint32_t hcs1 = xhci_read32(xhci->cap, 0x04U);
    xhci->max_slots = (uint8_t)(hcs1 & 0xffU);
    xhci->max_ports = (uint8_t)(hcs1 >> 24);
    if (xhci->max_slots > XHCI_MAX_SLOTS)
        xhci->max_slots = XHCI_MAX_SLOTS;
    if (xhci->max_ports > XHCI_MAX_PORTS)
        xhci->max_ports = XHCI_MAX_PORTS;
    xhci->context_size = (hcc1 & (1U << 2)) ? 64 : 32;
    xhci->hci_version = (uint16_t)(xhci_read32(cap, 0) >> 16);
    xhci->port_power_control = (hcc1 & (1U << 3)) ? 1 : 0;
    xhci->op = xhci->cap + cap_length;
    xhci->doorbell = xhci->cap + (xhci_read32(xhci->cap, 0x14U) & ~3U);
    xhci->runtime = xhci->cap + (xhci_read32(xhci->cap, 0x18U) & ~0x1fU);
    if (!cap_length || !xhci->max_slots || !xhci->max_ports) {
        kfree(xhci->alloc_ptr);
        return -ENODEV;
    }

    xhci->hcd.ops = &xhci_hcd_ops;
    xhci->hcd.hcd_dev = dev;
    xhci->hcd.max_ports = xhci->max_ports;
    xhci->hcd.priv = xhci;
    dev->drv_priv = xhci;

    /* Firmware commonly leaves MSI/MSI-X enabled on the xHC after booting
     * from USB.  A20OS uses the controller's INTx line, so clear the stale
     * MSI/MSI-X state and re-enable INTx in the PCI command register.  A
     * stale MSI-X table entry pointing at a reclaimed address is a direct
     * route to USBSTS.HSE on the first port-status event. */
    pci_msi_info_t msi_info;
    pci_msix_info_t msix_info;
    xhci->msi_present = pci_msi_capability(dev, &msi_info) == 0;
    xhci->msi_enabled = xhci->msi_present && msi_info.enabled;
    xhci->msix_present = pci_msix_capability(dev, &msix_info) == 0;
    xhci->msix_enabled = xhci->msix_present && msix_info.enabled;
    pci_msi_disable(dev);
    pci_msix_disable(dev);
    uint32_t pci_command = pci_cfg_read32(dev, 0x04U);
    if (pci_command & (1U << 10)) {
        pci_cfg_write32(dev, 0x04U, pci_command & ~(1U << 10));
        pci_command &= ~(1U << 10);
        kinfo("[XHCI] enabled INTx (cleared PCI Interrupt Disable)\n");
    }
    xhci->pci_command = pci_command;
    kinfo("[XHCI] firmware irq state: msi=%u/%u msix=%u/%u pci_cmd=0x%x\n",
          xhci->msi_present, xhci->msi_enabled,
          xhci->msix_present, xhci->msix_enabled, pci_command);

    /* Completion delivery: this controller's own INTx line, or the polling
     * fallback.  Registered before register_hcd() because start() is what arms
     * INTR0.IE, and IE stays clear until then, so no interrupt can arrive
     * against rings the driver has not primed yet.  The interrupt is a
     * level-triggered shared line on the same bus as every other INTx
     * function, so it is claimed shared for the same reason virtio-blk does. */
    if (!xhci_cmdline_poll()) {
        int irq = pci_intx_irq(dev);
        if (irq >= 0 &&
            request_irq((uint32_t)irq, xhci_irq_handler, IRQF_SHARED, xhci) == 0) {
            xhci->irq = irq;
            xhci->irq_registered = 1;
        } else {
            kinfo("[XHCI] IRQ %d registration failed; "
                  "using event-ring polling\n", irq);
        }
    } else {
        kinfo("[XHCI] a20.xhci.poll=1: skipping IRQ setup\n");
    }

    int result = usb_core_register_hcd(&xhci->hcd);
    if (result) {
        kerr("[XHCI] controller start failed: %d\n", result);
        if (xhci->irq_registered)
            free_irq((uint32_t)xhci->irq, xhci);
        kfree(xhci->alloc_ptr);
        dev->drv_priv = NULL;
        return result;
    }

    if (xhci_baytrail_route_ports(dev))
        kerr("[XHCI] Bay Trail port routing incomplete; inspect firmware USB routing\n");

kinfo("[XHCI] controller ready: MMIO=0x%lx slots=%u ports=%u irq=%d "
          "completion=%s\n",
          (unsigned long)xhci->cap, xhci->max_slots, xhci->max_ports,
          xhci->irq, xhci->irq_registered ? "interrupt" : "polling");
    return 0;
}

static int xhci_remove(device_t *dev)
{
    xhci_controller_t *xhci = (xhci_controller_t *)dev->drv_priv;
    if (!xhci)
        return 0;
    usb_core_unregister_hcd(&xhci->hcd);
    /* Mask the controller's interrupts before releasing the handler: a
     * completion racing the teardown must not find a freed ring.  Masking
     * under the same lock the handler takes is what makes "running = 0" and
     * "the line is quiet" one step rather than two. */
    uint64_t flags = spin_lock_irqsave(&xhci->lock);
    xhci->running = 0;
    xhci_write32(xhci->runtime + XHCI_INTERRUPTER_OFFSET, 0x00U, 0);
    spin_unlock_irqrestore(&xhci->lock, flags);
    if (xhci->irq_registered)
        free_irq((uint32_t)xhci->irq, xhci);
    xhci_write32(xhci->op, XHCI_USBCMD,
                 xhci_read32(xhci->op, XHCI_USBCMD) &
                 ~(XHCI_CMD_RUN | XHCI_CMD_INTE));
    dev->drv_priv = NULL;
    kfree(xhci->alloc_ptr);
    return 0;
}

static const device_id_t xhci_ids[] = {
    { .vendor = VENDOR_ANY, .device = DEVICE_ANY,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static driver_t xhci_driver = {
    .name = "xhci-hcd",
    .id_table = xhci_ids,
    /* This is a PCI HCD driver: restrict matching to the PCI bus.  With
     * .bus = NULL the wildcard id_table also matches virtio-mmio devices,
     * and xhci_pci_match() then dereferences a non-PCI plat_data. */
    .bus = &pci_bus,
    .match = xhci_pci_match,
    .probe = xhci_probe,
    .remove = xhci_remove,
    .class_type = DEV_CLASS_NONE,
};

DRIVER_REGISTER(xhci_driver);
