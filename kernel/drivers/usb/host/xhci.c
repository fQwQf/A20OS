/*
 * A20OS — xHCI host controller driver.
 *
 * Refactored from the original xhci_hid.c: the controller machinery (TRB
 * rings, slot/endpoint contexts, commands, control transfers, port reset,
 * enumeration primitives) is preserved, but made instance-based and exposed
 * through usb_hcd_ops so the USB core can drive any class of device.  HID
 * protocol parsing lives in the usb-hid class driver.
 */
#include "drivers/usb/usb.h"

#include "core/defs.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/string.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "mm/mm.h"
#include "abi/linux/errno.h"

#define XHCI_VENDOR_INTEL             0x8086U
#define XHCI_DEVICE_PANTHER_POINT     0x1e31U

#define XHCI_MAX_SLOTS                32U
#define XHCI_MAX_PORTS                32U
#define XHCI_RING_TRBS                64U
#define XHCI_EVENT_TRBS               128U
#define XHCI_WAIT_LOOPS               10000000U
#define XHCI_MAX_EPS                  32U

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
#define XHCI_STS_HALTED               (1U << 0)
#define XHCI_STS_CNR                  (1U << 11)

#define XHCI_PORT_CCS                 (1U << 0)
#define XHCI_PORT_PED                 (1U << 1)
#define XHCI_PORT_RESET               (1U << 4)
#define XHCI_PORT_POWER               (1U << 9)
#define XHCI_PORT_SPEED(v)            (((v) >> 10) & 0x0fU)
#define XHCI_PORT_CHANGE_BITS         0x00fe0000U
#define XHCI_PORT_WARM_RESET          (1U << 31)

#define XHCI_TRB_CYCLE                (1U << 0)
#define XHCI_TRB_ENT                  (1U << 1)
#define XHCI_TRB_ISP                  (1U << 2)
#define XHCI_TRB_CHAIN                (1U << 4)
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
#define XHCI_TRB_ADDRESS_DEVICE       11U
#define XHCI_TRB_CONFIGURE_ENDPOINT   12U
#define XHCI_TRB_EVALUATE_CONTEXT     13U
#define XHCI_TRB_TRANSFER_EVENT       32U
#define XHCI_TRB_COMMAND_EVENT        33U
#define XHCI_TRB_PORT_EVENT           34U

#define XHCI_CC_SUCCESS               1U
#define XHCI_CC_SHORT_PACKET          13U

#define USB_DIR_IN                    0x80U

typedef struct xhci_trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) xhci_trb_t;

typedef struct xhci_erst_entry {
    uint64_t address;
    uint32_t size;
    uint32_t reserved;
} __attribute__((packed)) xhci_erst_entry_t;

typedef struct xhci_ring {
    xhci_trb_t trbs[XHCI_RING_TRBS] ALIGNED(64);
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

typedef struct xhci_controller {
    void *alloc_ptr;            /* raw kmalloc base (for freeing) */
    usb_hcd_t hcd;
    uintptr_t cap;
    uintptr_t op;
    uintptr_t runtime;
    uintptr_t doorbell;
    uint8_t context_size;
    uint8_t max_slots;
    uint8_t max_ports;
    uint8_t event_cycle;
    uint16_t event_dequeue;
    uint8_t command_cycle;
    uint16_t command_enqueue;
    uint8_t running;
    spinlock_t lock;

    uint64_t dcbaa[XHCI_MAX_SLOTS + 1U] ALIGNED(64);
    uint8_t output_context[XHCI_MAX_SLOTS + 1U][2048] ALIGNED(64);
    uint8_t input_context[2112] ALIGNED(64);
    xhci_trb_t command_ring[XHCI_RING_TRBS] ALIGNED(64);
    xhci_trb_t event_ring[XHCI_EVENT_TRBS] ALIGNED(64);
    xhci_erst_entry_t erst ALIGNED(64);
    uint64_t scratchpad_array[32] ALIGNED(64);
    uint8_t scratchpads[32][PAGE_SIZE] ALIGNED(PAGE_SIZE);
    xhci_ring_t ep0_ring[XHCI_MAX_SLOTS + 1U];
    uint8_t control_buffer[512] ALIGNED(64);

    xhci_ep_t *eps;
    uint8_t ep_count;
} xhci_controller_t;

_Static_assert(sizeof(xhci_trb_t) == 16, "xHCI TRB must be 16 bytes");
_Static_assert(sizeof(usb_setup_packet_t) == 8, "USB setup packet must be 8 bytes");

static xhci_controller_t *xhci_alloc_controller(void);
static xhci_ep_t *xhci_alloc_ep(void);

static inline uint32_t xhci_read32(uintptr_t base, uint32_t offset) {
    return readl((const volatile void *)(base + offset));
}
static inline void xhci_write32(uintptr_t base, uint32_t offset, uint32_t value) {
    writel(value, (volatile void *)(base + offset));
}
static inline uint64_t xhci_read64(uintptr_t base, uint32_t offset) {
    uint32_t low = xhci_read32(base, offset);
    uint32_t high = xhci_read32(base, offset + 4U);
    return low | ((uint64_t)high << 32);
}
static inline void xhci_write64(uintptr_t base, uint32_t offset, uint64_t value) {
    xhci_write32(base, offset, (uint32_t)value);
    xhci_write32(base, offset + 4U, (uint32_t)(value >> 32));
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

static uint64_t xhci_ring_enqueue(xhci_ring_t *ring, uint64_t parameter,
                                  uint32_t status, uint32_t control) {
    uint16_t index = ring->enqueue;
    xhci_trb_t *trb = &ring->trbs[index];
    trb->parameter = parameter;
    trb->status = status;
    trb->control = control | (ring->cycle ? XHCI_TRB_CYCLE : 0U);
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
    return xhci->input_context + (size_t)index * xhci->context_size;
}
static void *xhci_output_context(xhci_controller_t *xhci, unsigned slot,
                                 unsigned index) {
    return xhci->output_context[slot] + (size_t)index * xhci->context_size;
}

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
    xhci_write64(xhci->runtime + 0x20U, 0x18U,
                 va_to_pa(&xhci->event_ring[xhci->event_dequeue]) | (1U << 3));
    return 1;
}

static xhci_ep_t *xhci_find_ep(xhci_controller_t *xhci, uint8_t slot,
                               uint8_t dci) {
    for (xhci_ep_t *e = xhci->eps; e; e = e->next)
        if (e->slot == slot && e->dci == dci)
            return e;
    return NULL;
}

static int xhci_wait_event(xhci_controller_t *xhci, uint8_t wanted_type,
                           uint64_t pointer, xhci_trb_t *result) {
    int any = 0;
    for (uint32_t i = 0; i < XHCI_WAIT_LOOPS; i++) {
        xhci_trb_t event;
        if (!xhci_next_event(xhci, &event)) {
            arch_cpu_relax();
            continue;
        }
        any++;
        uint8_t type = (uint8_t)((event.control >> 10) & 0x3fU);
        if (type == XHCI_TRB_TRANSFER_EVENT) {
            uint8_t slot = (uint8_t)(event.control >> 24);
            uint8_t dci = (uint8_t)((event.control >> 16) & 0x1fU);
            xhci_ep_t *ep = xhci_find_ep(xhci, slot, dci);
            if (ep && ep->pending) {
                /* Interrupt transfer completion while waiting for a control
                 * event: complete it and keep waiting. */
                usb_urb_t *urb = ep->pending;
                ep->pending = NULL;
                uint8_t cc = (uint8_t)(event.status >> 24);
                arch_dma_sync_for_cpu(urb->buf, urb->len);
                urb->status = (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET)
                                  ? 0 : -EIO;
                if (urb->complete)
                    urb->complete(urb);
                continue;
            }
        }
        if (type == wanted_type && (!pointer || (event.parameter & ~0x0fULL) ==
                                                   (pointer & ~0x0fULL))) {
            if (result)
                *result = event;
            uint8_t cc = (uint8_t)(event.status >> 24);
            return (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) ? 0 : -cc;
        }
    }
    kerr("[XHCI] wait_event timeout: wanted=%u seen=%d deq=%u\n",
         wanted_type, any, xhci->event_dequeue);
    return -ETIMEDOUT;
}

static int xhci_command(xhci_controller_t *xhci, uint64_t parameter,
                        uint32_t status, uint32_t control, xhci_trb_t *event) {
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
    return xhci_wait_event(xhci, XHCI_TRB_COMMAND_EVENT, pointer, event);
}

static int xhci_control(xhci_controller_t *xhci, uint8_t slot,
                        const usb_setup_packet_t *setup, void *data) {
    if (!slot || slot > xhci->max_slots)
        return -EINVAL;
    xhci_ring_t *ring = &xhci->ep0_ring[slot];
    uint64_t setup_value = 0;
    memcpy(&setup_value, setup, sizeof(*setup));
    uint32_t trt = setup->length ?
        ((setup->request_type & USB_DIR_IN) ? XHCI_TRB_TRT_IN : XHCI_TRB_TRT_OUT) : 0;
    xhci_ring_enqueue(ring, setup_value, 8,
                      XHCI_TRB_TYPE(XHCI_TRB_SETUP) | XHCI_TRB_IDT |
                      XHCI_TRB_CHAIN | trt);
    if (setup->length) {
        arch_dma_sync_for_device(data, setup->length);
        xhci_ring_enqueue(ring, va_to_pa(data), setup->length,
                          XHCI_TRB_TYPE(XHCI_TRB_DATA) | XHCI_TRB_ISP |
                          XHCI_TRB_CHAIN |
                          ((setup->request_type & USB_DIR_IN) ? XHCI_TRB_DIR_IN : 0));
    }
    uint32_t status_control = XHCI_TRB_TYPE(XHCI_TRB_STATUS) | XHCI_TRB_IOC;
    if (!setup->length || !(setup->request_type & USB_DIR_IN))
        status_control |= XHCI_TRB_DIR_IN;
    uint64_t status_pointer = xhci_ring_enqueue(ring, 0, 0, status_control);
    wmb();
    xhci_write32(xhci->doorbell, (uint32_t)slot * 4U, 1U);
    int result = xhci_wait_event(xhci, XHCI_TRB_TRANSFER_EVENT,
                                 status_pointer, NULL);
    if (result == 0 && setup->length && (setup->request_type & USB_DIR_IN))
        arch_dma_sync_for_cpu(data, setup->length);
    return result;
}

/* ------------------------------------------------------------------ */
/* Context helpers                                                     */
/* ------------------------------------------------------------------ */

static uint8_t xhci_default_mps(uint8_t speed) {
    if (speed >= 4)
        return 9;               /* SuperSpeed: 512 bytes = 2^9 */
    if (speed == 3)
        return 64;
    return 8;
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

static void xhci_fill_slot_context(xhci_controller_t *xhci, uint8_t port,
                                   uint8_t speed, uint8_t entries) {
    uint32_t *slot = xhci_input_context(xhci, 1);
    slot[0] = ((uint32_t)speed << 20) | ((uint32_t)entries << 27);
    slot[1] = (uint32_t)port << 16;
}

static void xhci_fill_ep_context(xhci_controller_t *xhci, uint8_t dci,
                                 xhci_ring_t *ring, uint8_t ep_type,
                                 uint16_t max_packet, uint8_t interval) {
    uint32_t *ep = xhci_input_context(xhci, (unsigned)dci + 1U);
    ep[0] = (uint32_t)interval << 16;
    ep[1] = (3U << 1) | ((uint32_t)ep_type << 3) |
            ((uint32_t)max_packet << 16);
    uint64_t dequeue = va_to_pa(ring->trbs) | 1U;
    ep[2] = (uint32_t)dequeue;
    ep[3] = (uint32_t)(dequeue >> 32);
    ep[4] = max_packet;
    if (ep_type == 3 || ep_type == 7 || ep_type == 1 || ep_type == 5)
        ep[4] |= (uint32_t)max_packet << 16;
}

/* ------------------------------------------------------------------ */
/* usb_hcd_ops                                                         */
/* ------------------------------------------------------------------ */

static int xhci_op_port_connected(usb_hcd_t *hcd, unsigned port)
{
    xhci_controller_t *xhci = hcd->priv;
    if (port < 1 || port > xhci->max_ports)
        return 0;
    uint32_t portsc = xhci_read32(xhci->op,
                                  XHCI_PORTSC_BASE +
                                  (port - 1U) * XHCI_PORTSC_STRIDE);
    return (portsc & XHCI_PORT_CCS) != 0;
}

static int xhci_op_reset_port(usb_hcd_t *hcd, unsigned port, uint8_t *speed) {
    xhci_controller_t *xhci = hcd->priv;
    uint32_t offset = XHCI_PORTSC_BASE + (port - 1U) * XHCI_PORTSC_STRIDE;
    uint32_t value = xhci_read32(xhci->op, offset);
    if (!(value & XHCI_PORT_CCS))
        return -ENODEV;
    uint32_t write = value & (XHCI_PORT_POWER | 0x1e0U);
    write |= XHCI_PORT_CHANGE_BITS;
    if (XHCI_PORT_SPEED(value) >= 4)
        write |= XHCI_PORT_WARM_RESET;
    else
        write |= XHCI_PORT_RESET;
    xhci_write32(xhci->op, offset, write);
    for (uint32_t i = 0; i < XHCI_WAIT_LOOPS; i++) {
        value = xhci_read32(xhci->op, offset);
        if (!(value & (XHCI_PORT_RESET | XHCI_PORT_WARM_RESET)) &&
            (value & XHCI_PORT_PED)) {
            *speed = (uint8_t)XHCI_PORT_SPEED(value);
            xhci_write32(xhci->op, offset,
                         (value & XHCI_PORT_POWER) |
                         (value & XHCI_PORT_CHANGE_BITS));
            return 0;
        }
        arch_cpu_relax();
    }
    return -ETIMEDOUT;
}

static int xhci_op_init_slot(usb_hcd_t *hcd, unsigned port, uint8_t speed,
                             uint8_t *slot_out) {
    xhci_controller_t *xhci = hcd->priv;
    xhci_trb_t event;
    int result = xhci_command(xhci, 0, 0,
                              XHCI_TRB_TYPE(XHCI_TRB_ENABLE_SLOT), &event);
    if (result)
        return result;
    uint8_t slot = (uint8_t)(event.control >> 24);
    if (!slot || slot > xhci->max_slots)
        return -EIO;

    memset(xhci->input_context, 0, sizeof(xhci->input_context));
    uint32_t *control = xhci_input_context(xhci, 0);
    control[1] = (1U << 0) | (1U << 1);
    xhci_fill_slot_context(xhci, (uint8_t)port, speed, 1);
    xhci_ring_init(&xhci->ep0_ring[slot]);
    xhci_fill_ep_context(xhci, 1, &xhci->ep0_ring[slot], 4,
                         xhci_mps_value(speed, xhci_default_mps(speed)), 0);
    memset(xhci->output_context[slot], 0, sizeof(xhci->output_context[slot]));
    xhci->dcbaa[slot] = va_to_pa(xhci->output_context[slot]);
    arch_dma_sync_for_device(xhci->output_context[slot],
                             sizeof(xhci->output_context[slot]));
    arch_dma_sync_for_device(xhci->input_context, sizeof(xhci->input_context));
    arch_dma_sync_for_device(xhci->dcbaa, sizeof(xhci->dcbaa));
    result = xhci_command(xhci, va_to_pa(xhci->input_context), 0,
                          XHCI_TRB_TYPE(XHCI_TRB_ADDRESS_DEVICE) |
                          ((uint32_t)slot << 24), NULL);
    if (result == 0)
        arch_dma_sync_for_cpu(xhci->output_context[slot],
                              sizeof(xhci->output_context[slot]));
    *slot_out = slot;
    return result;
}

static int xhci_op_update_ep0_mps(usb_hcd_t *hcd, uint8_t slot,
                                  uint16_t max_packet) {
    xhci_controller_t *xhci = hcd->priv;
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
    return xhci_command(xhci, va_to_pa(xhci->input_context), 0,
                        XHCI_TRB_TYPE(XHCI_TRB_EVALUATE_CONTEXT) |
                        ((uint32_t)slot << 24), NULL);
}

static int xhci_op_control(usb_hcd_t *hcd, uint8_t slot,
                           const usb_setup_packet_t *setup, void *data) {
    xhci_controller_t *xhci = hcd->priv;
    if (setup->length && data) {
        if (setup->length > sizeof(xhci->control_buffer))
            return -EINVAL;
        if (!(setup->request_type & USB_DIR_IN))
            memcpy(xhci->control_buffer, data, setup->length);
        int r = xhci_control(xhci, slot, setup, xhci->control_buffer);
        if (r == 0 && (setup->request_type & USB_DIR_IN))
            memcpy(data, xhci->control_buffer, setup->length);
        return r;
    }
    return xhci_control(xhci, slot, setup, NULL);
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
    int r = xhci_control(xhci, slot, &setup, xhci->control_buffer);
    if (r == 0)
        memcpy(buf, xhci->control_buffer, len);
    return r;
}

static int xhci_op_configure_endpoint(usb_hcd_t *hcd, usb_device_t *dev,
                                      uint8_t addr, uint8_t ep_type,
                                      uint16_t max_packet, uint8_t interval) {
    xhci_controller_t *xhci = hcd->priv;
    uint8_t number = addr & 0x0fU;
    if (!number)
        return -EINVAL;
    uint8_t dci = (uint8_t)((addr & USB_DIR_IN) ? number * 2U + 1U : number * 2U);
    if (dci >= 32 || xhci->ep_count >= XHCI_MAX_EPS)
        return -EINVAL;

    xhci_ep_t *ep = xhci_alloc_ep();
    if (!ep)
        return -ENOMEM;
    xhci_ring_init(&ep->ring);
    ep->slot = dev->slot;
    ep->dci = dci;
    ep->addr = addr;

    memset(xhci->input_context, 0, sizeof(xhci->input_context));
    arch_dma_sync_for_cpu(xhci->output_context[dev->slot],
                          sizeof(xhci->output_context[dev->slot]));
    uint32_t *control = xhci_input_context(xhci, 0);
    uint32_t *out_slot = xhci_output_context(xhci, dev->slot, 0);
    uint32_t *in_slot = xhci_input_context(xhci, 1);
    memcpy(in_slot, out_slot, xhci->context_size);
    control[1] = 1U;
    control[1] |= 1U << dci;
    xhci_fill_ep_context(xhci, dci, &ep->ring, ep_type, max_packet,
                         xhci_interval(dev->speed, interval));
    in_slot[0] = (in_slot[0] & ~(0x1fU << 27)) | ((uint32_t)dci << 27);
    arch_dma_sync_for_device(xhci->input_context, sizeof(xhci->input_context));
    int result = xhci_command(xhci, va_to_pa(xhci->input_context), 0,
                              XHCI_TRB_TYPE(XHCI_TRB_CONFIGURE_ENDPOINT) |
                              ((uint32_t)dev->slot << 24), NULL);
    if (result) {
        kfree(ep->alloc_ptr);
        return result;
    }
    ep->next = xhci->eps;
    xhci->eps = ep;
    xhci->ep_count++;
    return 0;
}

static int xhci_op_submit_interrupt(usb_hcd_t *hcd, usb_urb_t *urb) {
    xhci_controller_t *xhci = hcd->priv;
    if (!urb || !urb->ep)
        return -EINVAL;
    uint8_t addr = urb->ep->addr;
    uint8_t number = addr & 0x0fU;
    uint8_t dci = (uint8_t)((addr & USB_DIR_IN) ? number * 2U + 1U : number * 2U);
    xhci_ep_t *ep = xhci_find_ep(xhci, urb->dev->slot, dci);
    if (!ep)
        return -ENODEV;
    if (ep->pending)
        return -EBUSY;
    memset(urb->buf, 0, urb->len);
    arch_dma_sync_for_device(urb->buf, urb->len);
    xhci_ring_enqueue(&ep->ring, va_to_pa(urb->buf), (uint32_t)urb->len,
                      XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_ISP |
                      XHCI_TRB_IOC);
    ep->pending = urb;
    wmb();
    xhci_write32(xhci->doorbell, (uint32_t)urb->dev->slot * 4U, dci);
    return 0;
}

static int xhci_op_abort_slot(usb_hcd_t *hcd, uint8_t slot) {
    xhci_controller_t *xhci = hcd->priv;
    xhci_ep_t **pp = &xhci->eps;
    while (*pp) {
        xhci_ep_t *e = *pp;
        if (e->slot == slot) {
            *pp = e->next;
            xhci->ep_count--;
            kfree(e->alloc_ptr);
        } else {
            pp = &e->next;
        }
    }
    return 0;
}

static int xhci_op_poll(usb_hcd_t *hcd) {
    xhci_controller_t *xhci = hcd->priv;
    xhci_trb_t event;
    int handled = 0;
    while (xhci_next_event(xhci, &event)) {
        uint8_t type = (uint8_t)((event.control >> 10) & 0x3fU);
        if (type == XHCI_TRB_TRANSFER_EVENT) {
            uint8_t slot = (uint8_t)(event.control >> 24);
            uint8_t dci = (uint8_t)((event.control >> 16) & 0x1fU);
            xhci_ep_t *ep = xhci_find_ep(xhci, slot, dci);
            if (ep && ep->pending) {
                usb_urb_t *urb = ep->pending;
                ep->pending = NULL;
                uint8_t cc = (uint8_t)(event.status >> 24);
                arch_dma_sync_for_cpu(urb->buf, urb->len);
                urb->status = (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET)
                                  ? 0 : -EIO;
                if (urb->complete)
                    urb->complete(urb);
                handled = 1;
            }
        }
    }
    return handled;
}

static int xhci_op_start(usb_hcd_t *hcd) {
    xhci_controller_t *xhci = hcd->priv;
    /* Firmware ownership handoff. */
    uint32_t hcc = xhci_read32(xhci->cap, 0x10U);
    uint32_t offset = (hcc >> 16) * 4U;
    for (unsigned limit = 0; offset && limit < 64; limit++) {
        uint32_t cap = xhci_read32(xhci->cap, offset);
        uint8_t id = (uint8_t)cap;
        uint32_t next = ((cap >> 8) & 0xffU) * 4U;
        if (id == 1) {
            xhci_write32(xhci->cap, offset, cap | (1U << 24));
            for (uint32_t i = 0; i < XHCI_WAIT_LOOPS; i++) {
                if (!(xhci_read32(xhci->cap, offset) & (1U << 16)))
                    break;
                arch_cpu_relax();
            }
            break;
        }
        offset = next ? offset + next : 0;
    }

    xhci_write32(xhci->op, XHCI_USBCMD,
                 xhci_read32(xhci->op, XHCI_USBCMD) & ~XHCI_CMD_RUN);
    if (xhci_wait32(xhci->op, XHCI_USBSTS, XHCI_STS_HALTED, XHCI_STS_HALTED) != 0)
        return -ETIMEDOUT;
    xhci_write32(xhci->op, XHCI_USBCMD, XHCI_CMD_RESET);
    if (xhci_wait32(xhci->op, XHCI_USBCMD, XHCI_CMD_RESET, 0) != 0 ||
        xhci_wait32(xhci->op, XHCI_USBSTS, XHCI_STS_CNR, 0) != 0)
        return -ETIMEDOUT;
    if (!(xhci_read32(xhci->op, XHCI_PAGESIZE) & 1U))
        return -ENOSYS;

    memset(xhci->dcbaa, 0, sizeof(xhci->dcbaa));
    uint32_t hcs2 = xhci_read32(xhci->cap, 0x08U);
    unsigned scratchpads = (((hcs2 >> 21) & 0x1fU) << 5) |
                           ((hcs2 >> 27) & 0x1fU);
    if (scratchpads > ARRAY_SIZE(xhci->scratchpad_array))
        return -ENOSYS;
    if (scratchpads) {
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

    xhci_write64(xhci->op, XHCI_DCBAAP, va_to_pa(xhci->dcbaa));
    xhci_write64(xhci->op, XHCI_CRCR, va_to_pa(xhci->command_ring) | 1U);
    uintptr_t ir0 = xhci->runtime + 0x20U;
    xhci_write32(ir0, 0x00U, 0x1U);  /* Polling: clear IP, leave IE disabled. */
    xhci_write32(ir0, 0x08U, 1U);
    xhci_write64(ir0, 0x10U, va_to_pa(&xhci->erst));
    xhci_write64(ir0, 0x18U, va_to_pa(xhci->event_ring));
    xhci_write32(xhci->op, XHCI_CONFIG, xhci->max_slots);
    xhci_write32(xhci->op, XHCI_USBCMD, XHCI_CMD_RUN);
    if (xhci_wait32(xhci->op, XHCI_USBSTS, XHCI_STS_HALTED, 0) != 0)
        return -ETIMEDOUT;

    xhci->running = 1;
    kinfo("[XHCI] controller running: slots=%u ports=%u context=%u sts=0x%x hcs2=0x%x\n",
          xhci->max_slots, xhci->max_ports, xhci->context_size,
          xhci_read32(xhci->op, XHCI_USBSTS), xhci_read32(xhci->cap, 0x08U));
    return 0;
}

static const usb_hcd_ops_t xhci_hcd_ops = {
    .start = xhci_op_start,
    .poll = xhci_op_poll,
    .port_connected = xhci_op_port_connected,
    .reset_port = xhci_op_reset_port,
    .init_slot = xhci_op_init_slot,
    .update_ep0_mps = xhci_op_update_ep0_mps,
    .control = xhci_op_control,
    .get_descriptor = xhci_op_get_descriptor,
    .configure_endpoint = xhci_op_configure_endpoint,
    .submit_interrupt = xhci_op_submit_interrupt,
    .abort_slot = xhci_op_abort_slot,
};

/* xHCI DMA structures (rings, contexts, DCBAA) require 64-byte alignment.
 * kmalloc only guarantees 8/16-byte alignment, so allocate with a manual
 * 64-byte alignment and remember the raw base for freeing. */
#define XHCI_DMA_ALIGN 64UL

/* ------------------------------------------------------------------ */
/* PCI driver                                                          */
/* ------------------------------------------------------------------ */

static xhci_controller_t *xhci_alloc_controller(void)
{
    size_t sz = sizeof(xhci_controller_t) + XHCI_DMA_ALIGN - 1UL;
    void *raw = kmalloc(sz);
    if (!raw)
        return NULL;
    xhci_controller_t *xhci = (xhci_controller_t *)
        (((uintptr_t)raw + (XHCI_DMA_ALIGN - 1UL)) & ~(XHCI_DMA_ALIGN - 1UL));
    xhci->alloc_ptr = raw;
    memset(xhci, 0, sizeof(*xhci));
    return xhci;
}

static xhci_ep_t *xhci_alloc_ep(void)
{
    size_t sz = sizeof(xhci_ep_t) + XHCI_DMA_ALIGN - 1UL;
    void *raw = kcalloc(1, sz);
    if (!raw)
        return NULL;
    xhci_ep_t *ep = (xhci_ep_t *)
        (((uintptr_t)raw + (XHCI_DMA_ALIGN - 1UL)) & ~(XHCI_DMA_ALIGN - 1UL));
    ep->alloc_ptr = raw;
    return ep;
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

static int xhci_probe(device_t *dev)
{
    if (pci_enable_and_assign_bars(dev) != 0)
        return -ENODEV;
    resource_t *bar = pci_get_bar_resource(dev, 0);
    if (!bar || bar->end < bar->start || bar->end - bar->start + 1U < 0x1000U)
        return -ENODEV;

    xhci_controller_t *xhci = xhci_alloc_controller();
    if (!xhci)
        return -ENOMEM;
    spin_init(&xhci->lock);
    xhci->cap = (uintptr_t)bar->start;
    uint8_t cap_length = readb((const volatile void *)xhci->cap);
    uint32_t hcs1 = xhci_read32(xhci->cap, 0x04U);
    uint32_t hcc1 = xhci_read32(xhci->cap, 0x10U);
    xhci->max_slots = (uint8_t)(hcs1 & 0xffU);
    xhci->max_ports = (uint8_t)(hcs1 >> 24);
    if (xhci->max_slots > XHCI_MAX_SLOTS)
        xhci->max_slots = XHCI_MAX_SLOTS;
    if (xhci->max_ports > XHCI_MAX_PORTS)
        xhci->max_ports = XHCI_MAX_PORTS;
    xhci->context_size = (hcc1 & (1U << 2)) ? 64 : 32;
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

    int result = usb_core_register_hcd(&xhci->hcd);
    if (result) {
        kerr("[XHCI] controller start failed: %d\n", result);
        kfree(xhci->alloc_ptr);
        dev->drv_priv = NULL;
        return result;
    }

    kinfo("[XHCI] controller ready: MMIO=0x%lx slots=%u ports=%u\n",
          (unsigned long)xhci->cap, xhci->max_slots, xhci->max_ports);
    return 0;
}

static int xhci_remove(device_t *dev)
{
    xhci_controller_t *xhci = (xhci_controller_t *)dev->drv_priv;
    if (!xhci)
        return 0;
    usb_core_unregister_hcd(&xhci->hcd);
    xhci_write32(xhci->op, XHCI_USBCMD,
                 xhci_read32(xhci->op, XHCI_USBCMD) & ~XHCI_CMD_RUN);
    xhci->running = 0;
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
    .bus = NULL,
    .match = xhci_pci_match,
    .probe = xhci_probe,
    .remove = xhci_remove,
    .class_type = DEV_CLASS_NONE,
};

DRIVER_REGISTER(xhci_driver);
