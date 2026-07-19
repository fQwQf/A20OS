#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drivers/input/virtio_input.h"
#include "abi/linux/errno.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/string.h"
#include "mm/mm.h"

/* USB xHCI 1.2 plus HID 1.11 boot-protocol keyboard/mouse support.  VBox ARM
 * exposes its USB keyboard and pointing device behind Intel 8086:1e31. */

#define XHCI_VENDOR_INTEL             0x8086U
#define XHCI_DEVICE_PANTHER_POINT     0x1e31U

#define XHCI_MAX_SLOTS                32U
#define XHCI_MAX_PORTS                32U
#define XHCI_MAX_HID_ENDPOINTS        8U
#define XHCI_RING_TRBS                64U
#define XHCI_EVENT_TRBS               128U
#define XHCI_REPORT_SIZE              64U
#define XHCI_INPUT_EVENTS             256U
#define XHCI_WAIT_LOOPS               10000000U

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
#define USB_TYPE_STANDARD             0x00U
#define USB_TYPE_CLASS                0x20U
#define USB_RECIP_DEVICE              0x00U
#define USB_RECIP_INTERFACE           0x01U
#define USB_REQ_GET_DESCRIPTOR        6U
#define USB_REQ_SET_CONFIGURATION     9U
#define USB_REQ_SET_IDLE              10U
#define USB_REQ_SET_PROTOCOL          11U
#define USB_DT_DEVICE                 1U
#define USB_DT_CONFIG                 2U
#define USB_DT_INTERFACE              4U
#define USB_DT_ENDPOINT               5U
#define USB_CLASS_HID                 3U
#define VBOX_USB_VENDOR               0x80eeU
#define VBOX_USB_TABLET               0x0021U

#define SYN_REPORT                    0x00U
#define REL_X                         0x00U
#define REL_Y                         0x01U
#define REL_WHEEL                     0x08U
#define ABS_X                         0x00U
#define ABS_Y                         0x01U
#define BTN_LEFT                      0x110U
#define BTN_RIGHT                     0x111U
#define BTN_MIDDLE                    0x112U

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

typedef struct usb_setup_packet {
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} __attribute__((packed)) usb_setup_packet_t;

typedef struct usb_device_descriptor {
    uint8_t length;
    uint8_t type;
    uint16_t usb;
    uint8_t dev_class;
    uint8_t dev_subclass;
    uint8_t dev_protocol;
    uint8_t max_packet0;
    uint16_t vendor;
    uint16_t product;
    uint16_t device;
    uint8_t manufacturer;
    uint8_t product_string;
    uint8_t serial;
    uint8_t configurations;
} __attribute__((packed)) usb_device_descriptor_t;

typedef struct xhci_ring {
    xhci_trb_t trbs[XHCI_RING_TRBS] ALIGNED(64);
    uint16_t enqueue;
    uint8_t cycle;
} xhci_ring_t;

typedef struct xhci_hid_endpoint {
    xhci_ring_t ring;
    uint8_t report[XHCI_REPORT_SIZE] ALIGNED(64);
    uint8_t previous[XHCI_REPORT_SIZE];
    uint8_t slot_id;
    uint8_t dci;
    uint8_t interface_number;
    uint8_t protocol;
    uint8_t report_size;
    uint8_t pending;
    uint8_t valid;
} xhci_hid_endpoint_t;

typedef struct xhci_controller {
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
    /* An xHCI transfer ring belongs to an endpoint, not to the controller.
     * Reusing one EP0 ring across slots lets a late completion from one USB
     * device satisfy a control request for another device at the same TRB
     * address. */
    xhci_ring_t ep0_ring[XHCI_MAX_SLOTS + 1U];
    uint8_t control_buffer[512] ALIGNED(64);
    xhci_hid_endpoint_t hid[XHCI_MAX_HID_ENDPOINTS];
    uint8_t hid_count;

    struct input_event input[XHCI_INPUT_EVENTS];
    uint16_t input_head;
    uint16_t input_tail;
} xhci_controller_t;

static xhci_controller_t g_xhci;

_Static_assert(sizeof(xhci_trb_t) == 16, "xHCI TRB must be 16 bytes");
_Static_assert(sizeof(usb_setup_packet_t) == 8, "USB setup packet must be 8 bytes");

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

static void xhci_emit(xhci_controller_t *xhci, uint16_t type, uint16_t code,
                      int32_t value) {
    uint16_t next = (uint16_t)((xhci->input_head + 1U) % XHCI_INPUT_EVENTS);
    if (next == xhci->input_tail)
        return;
    struct input_event *event = &xhci->input[xhci->input_head];
    event->time_sec = 0;
    event->time_usec = 0;
    event->type = type;
    event->code = code;
    event->value = value;
    xhci->input_head = next;
}

static const uint16_t hid_usage_to_linux[0x74] = {
    [0x04] = 30, [0x05] = 48, [0x06] = 46, [0x07] = 32,
    [0x08] = 18, [0x09] = 33, [0x0a] = 34, [0x0b] = 35,
    [0x0c] = 23, [0x0d] = 36, [0x0e] = 37, [0x0f] = 38,
    [0x10] = 50, [0x11] = 49, [0x12] = 24, [0x13] = 25,
    [0x14] = 16, [0x15] = 19, [0x16] = 31, [0x17] = 20,
    [0x18] = 22, [0x19] = 47, [0x1a] = 17, [0x1b] = 45,
    [0x1c] = 21, [0x1d] = 44,
    [0x1e] = 2, [0x1f] = 3, [0x20] = 4, [0x21] = 5,
    [0x22] = 6, [0x23] = 7, [0x24] = 8, [0x25] = 9,
    [0x26] = 10, [0x27] = 11,
    [0x28] = 28, [0x29] = 1, [0x2a] = 14, [0x2b] = 15,
    [0x2c] = 57, [0x2d] = 12, [0x2e] = 13, [0x2f] = 26,
    [0x30] = 27, [0x31] = 43, [0x32] = 43, [0x33] = 39,
    [0x34] = 40, [0x35] = 41, [0x36] = 51, [0x37] = 52,
    [0x38] = 53, [0x39] = 58,
    [0x3a] = 59, [0x3b] = 60, [0x3c] = 61, [0x3d] = 62,
    [0x3e] = 63, [0x3f] = 64, [0x40] = 65, [0x41] = 66,
    [0x42] = 67, [0x43] = 68, [0x44] = 87, [0x45] = 88,
    [0x46] = 99, [0x47] = 70, [0x48] = 119, [0x49] = 110,
    [0x4a] = 102, [0x4b] = 104, [0x4c] = 111, [0x4d] = 107,
    [0x4e] = 109, [0x4f] = 106, [0x50] = 105, [0x51] = 108,
    [0x52] = 103, [0x53] = 69, [0x54] = 98, [0x55] = 55,
    [0x56] = 74, [0x57] = 78, [0x58] = 96, [0x59] = 79,
    [0x5a] = 80, [0x5b] = 81, [0x5c] = 75, [0x5d] = 76,
    [0x5e] = 77, [0x5f] = 71, [0x60] = 72, [0x61] = 73,
    [0x62] = 82, [0x63] = 83, [0x65] = 127,
};

static int hid_key_present(const uint8_t report[8], uint8_t usage) {
    for (unsigned i = 2; i < 8; i++)
        if (report[i] == usage)
            return 1;
    return 0;
}

static void xhci_keyboard_report(xhci_controller_t *xhci,
                                 xhci_hid_endpoint_t *hid) {
    static const uint16_t modifier_codes[8] = { 29, 42, 56, 125, 97, 54, 100, 126 };
    const uint8_t *now = hid->report;
    const uint8_t *old = hid->previous;
    int changed = 0;
    uint8_t modifiers = now[0] ^ old[0];
    for (unsigned bit = 0; bit < 8; bit++) {
        if (modifiers & (1U << bit)) {
            xhci_emit(xhci, EV_KEY, modifier_codes[bit], !!(now[0] & (1U << bit)));
            changed = 1;
        }
    }
    for (unsigned i = 2; i < 8; i++) {
        uint8_t usage = old[i];
        if (usage > 3 && !hid_key_present(now, usage) &&
            usage < ARRAY_SIZE(hid_usage_to_linux) && hid_usage_to_linux[usage]) {
            xhci_emit(xhci, EV_KEY, hid_usage_to_linux[usage], 0);
            changed = 1;
        }
    }
    for (unsigned i = 2; i < 8; i++) {
        uint8_t usage = now[i];
        if (usage > 3 && !hid_key_present(old, usage) &&
            usage < ARRAY_SIZE(hid_usage_to_linux) && hid_usage_to_linux[usage]) {
            xhci_emit(xhci, EV_KEY, hid_usage_to_linux[usage], 1);
            changed = 1;
        }
    }
    if (changed)
        xhci_emit(xhci, EV_SYN, SYN_REPORT, 0);
    memcpy(hid->previous, now, 8);
}

static void xhci_mouse_report(xhci_controller_t *xhci,
                              xhci_hid_endpoint_t *hid) {
    const uint8_t *now = hid->report;
    const uint8_t *old = hid->previous;
    static const uint16_t buttons[3] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
    int changed = 0;
    for (unsigned bit = 0; bit < 3; bit++) {
        if ((now[0] ^ old[0]) & (1U << bit)) {
            xhci_emit(xhci, EV_KEY, buttons[bit], !!(now[0] & (1U << bit)));
            changed = 1;
        }
    }
    if ((int8_t)now[1]) {
        xhci_emit(xhci, EV_REL, REL_X, (int8_t)now[1]);
        changed = 1;
    }
    if ((int8_t)now[2]) {
        xhci_emit(xhci, EV_REL, REL_Y, (int8_t)now[2]);
        changed = 1;
    }
    if (hid->report_size >= 4 && (int8_t)now[3]) {
        xhci_emit(xhci, EV_REL, REL_WHEEL, (int8_t)now[3]);
        changed = 1;
    }
    if (changed)
        xhci_emit(xhci, EV_SYN, SYN_REPORT, 0);
    memcpy(hid->previous, now, hid->report_size);
}

static void xhci_tablet_report(xhci_controller_t *xhci,
                               xhci_hid_endpoint_t *hid) {
    const uint8_t *now = hid->report;
    const uint8_t *old = hid->previous;
    static const uint16_t buttons[3] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
    for (unsigned bit = 0; bit < 3; bit++) {
        if ((now[0] ^ old[0]) & (1U << bit))
            xhci_emit(xhci, EV_KEY, buttons[bit], !!(now[0] & (1U << bit)));
    }
    /* VBox USB Tablet: buttons, wheels, padding, then 16-bit absolute X/Y. */
    uint16_t x = (uint16_t)now[4] | ((uint16_t)now[5] << 8);
    uint16_t y = (uint16_t)now[6] | ((uint16_t)now[7] << 8);
    xhci_emit(xhci, EV_ABS, ABS_X, x);
    xhci_emit(xhci, EV_ABS, ABS_Y, y);
    if ((int8_t)now[1])
        xhci_emit(xhci, EV_REL, REL_WHEEL, (int8_t)now[1]);
    xhci_emit(xhci, EV_SYN, SYN_REPORT, 0);
    memcpy(hid->previous, now, hid->report_size);
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

static void xhci_submit_interrupt(xhci_controller_t *xhci,
                                  xhci_hid_endpoint_t *hid) {
    memset(hid->report, 0, hid->report_size);
    arch_dma_sync_for_device(hid->report, hid->report_size);
    xhci_ring_enqueue(&hid->ring, va_to_pa(hid->report), hid->report_size,
                      XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_ISP | XHCI_TRB_IOC);
    hid->pending = 1;
    wmb();
    xhci_write32(xhci->doorbell, (uint32_t)hid->slot_id * 4U, hid->dci);
}

static void xhci_dispatch_transfer(xhci_controller_t *xhci,
                                   const xhci_trb_t *event) {
    uint8_t slot = (uint8_t)(event->control >> 24);
    uint8_t dci = (uint8_t)((event->control >> 16) & 0x1fU);
    uint8_t cc = (uint8_t)(event->status >> 24);
    for (unsigned i = 0; i < xhci->hid_count; i++) {
        xhci_hid_endpoint_t *hid = &xhci->hid[i];
        if (!hid->valid || hid->slot_id != slot || hid->dci != dci)
            continue;
        hid->pending = 0;
        if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) {
            arch_dma_sync_for_cpu(hid->report, hid->report_size);
            if (hid->protocol == 1)
                xhci_keyboard_report(xhci, hid);
            else if (hid->protocol == 2)
                xhci_mouse_report(xhci, hid);
            else if (hid->protocol == 3)
                xhci_tablet_report(xhci, hid);
        } else {
            kerr("[XHCI] HID transfer failed: slot=%u dci=%u cc=%u\n",
                 slot, dci, cc);
        }
        xhci_submit_interrupt(xhci, hid);
        return;
    }
}

static void xhci_poll_locked(xhci_controller_t *xhci) {
    xhci_trb_t event;
    while (xhci_next_event(xhci, &event)) {
        uint8_t type = (uint8_t)((event.control >> 10) & 0x3fU);
        if (type == XHCI_TRB_TRANSFER_EVENT)
            xhci_dispatch_transfer(xhci, &event);
    }
}

static int xhci_wait_event(xhci_controller_t *xhci, uint8_t wanted_type,
                           uint64_t pointer, xhci_trb_t *result) {
    for (uint32_t i = 0; i < XHCI_WAIT_LOOPS; i++) {
        xhci_trb_t event;
        if (!xhci_next_event(xhci, &event)) {
            arch_cpu_relax();
            continue;
        }
        uint8_t type = (uint8_t)((event.control >> 10) & 0x3fU);
        if (type == XHCI_TRB_TRANSFER_EVENT) {
            uint8_t slot = (uint8_t)(event.control >> 24);
            uint8_t dci = (uint8_t)((event.control >> 16) & 0x1fU);
            int hid_event = 0;
            for (unsigned h = 0; h < xhci->hid_count; h++)
                hid_event |= xhci->hid[h].valid && xhci->hid[h].slot_id == slot &&
                             xhci->hid[h].dci == dci;
            if (hid_event) {
                xhci_dispatch_transfer(xhci, &event);
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
        /* Clean and invalidate before both DMA directions.  In particular,
         * GET_DESCRIPTOR follows a memset of the receive buffer; leaving
         * those dirty cache lines live can hide the controller's response. */
        arch_dma_sync_for_device(data, setup->length);
            xhci_ring_enqueue(ring, va_to_pa(data), setup->length,
                          XHCI_TRB_TYPE(XHCI_TRB_DATA) | XHCI_TRB_ISP |
                          XHCI_TRB_CHAIN |
                          ((setup->request_type & USB_DIR_IN) ? XHCI_TRB_DIR_IN : 0));
        kinfo("[XHCI] control slot=%u dma=0x%lx pa=0x%lx len=%u\n",
              slot, (unsigned long)va_to_pa(data),
              (unsigned long)va_to_pa(data), setup->length);
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

static uint8_t xhci_default_mps(uint8_t speed) {
    if (speed >= 4)
        return 9; /* SuperSpeed encodes 512 bytes as 2^9 in bMaxPacketSize0. */
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
    /* Periodic endpoints require Max ESIT Payload; it is reserved for EP0. */
    ep[4] = max_packet;
    if (ep_type == 3 || ep_type == 7 || ep_type == 1 || ep_type == 5)
        ep[4] |= (uint32_t)max_packet << 16;
}

static int xhci_reset_port(xhci_controller_t *xhci, unsigned port,
                           uint8_t *speed) {
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
            /* PORTSC.PED is RW1C: echoing the read value would immediately
             * disable the port we just reset.  Write only power and W1C bits. */
            xhci_write32(xhci->op, offset,
                         (value & XHCI_PORT_POWER) |
                         (value & XHCI_PORT_CHANGE_BITS));
            return 0;
        }
        arch_cpu_relax();
    }
    kerr("[XHCI] port %u reset timeout (PORTSC=0x%x)\n", port, value);
    return -ETIMEDOUT;
}

static int xhci_enable_slot(xhci_controller_t *xhci, uint8_t *slot_id) {
    xhci_trb_t event;
    int result = xhci_command(xhci, 0, 0,
                              XHCI_TRB_TYPE(XHCI_TRB_ENABLE_SLOT), &event);
    if (result)
        return result;
    *slot_id = (uint8_t)(event.control >> 24);
    return *slot_id ? 0 : -EIO;
}

static int xhci_address_device(xhci_controller_t *xhci, uint8_t slot,
                               uint8_t port, uint8_t speed) {
    memset(xhci->input_context, 0, sizeof(xhci->input_context));
    uint32_t *control = xhci_input_context(xhci, 0);
    control[1] = (1U << 0) | (1U << 1);
    xhci_fill_slot_context(xhci, port, speed, 1);
    xhci_ring_init(&xhci->ep0_ring[slot]);
    xhci_fill_ep_context(xhci, 1, &xhci->ep0_ring[slot], 4,
                         xhci_mps_value(speed, xhci_default_mps(speed)), 0);
    memset(xhci->output_context[slot], 0, sizeof(xhci->output_context[slot]));
    xhci->dcbaa[slot] = va_to_pa(xhci->output_context[slot]);
    arch_dma_sync_for_device(xhci->output_context[slot],
                             sizeof(xhci->output_context[slot]));
    arch_dma_sync_for_device(xhci->input_context, sizeof(xhci->input_context));
    arch_dma_sync_for_device(xhci->dcbaa, sizeof(xhci->dcbaa));
    int result = xhci_command(xhci, va_to_pa(xhci->input_context), 0,
                              XHCI_TRB_TYPE(XHCI_TRB_ADDRESS_DEVICE) |
                              ((uint32_t)slot << 24), NULL);
    if (result == 0)
        arch_dma_sync_for_cpu(xhci->output_context[slot],
                              sizeof(xhci->output_context[slot]));
    return result;
}

static int xhci_update_ep0_mps(xhci_controller_t *xhci, uint8_t slot,
                               uint16_t max_packet) {
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

static int xhci_get_descriptor(xhci_controller_t *xhci, uint8_t slot,
                               uint8_t type, uint16_t length) {
    usb_setup_packet_t setup = {
        .request_type = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .request = USB_REQ_GET_DESCRIPTOR,
        .value = (uint16_t)type << 8,
        .index = 0,
        .length = length,
    };
    memset(xhci->control_buffer, 0xa5, length);
    int result = xhci_control(xhci, slot, &setup, xhci->control_buffer);
    kinfo("[XHCI] descriptor slot=%u type=%u len=%u result=%d bytes="
          "%02x %02x %02x %02x %02x %02x %02x %02x "
          "va=0x%lx dma=0x%lx\n", slot, type, length, result,
          xhci->control_buffer[0], xhci->control_buffer[1],
          xhci->control_buffer[2], xhci->control_buffer[3],
          xhci->control_buffer[4], xhci->control_buffer[5],
          xhci->control_buffer[6], xhci->control_buffer[7],
          (unsigned long)xhci->control_buffer,
          (unsigned long)va_to_pa(xhci->control_buffer));
    return result;
}

static int xhci_no_data_request(xhci_controller_t *xhci, uint8_t slot,
                                uint8_t request_type, uint8_t request,
                                uint16_t value, uint16_t index) {
    usb_setup_packet_t setup = {
        .request_type = request_type,
        .request = request,
        .value = value,
        .index = index,
        .length = 0,
    };
    return xhci_control(xhci, slot, &setup, NULL);
}

typedef struct xhci_hid_candidate {
    uint8_t interface_number;
    uint8_t protocol;
    uint8_t endpoint;
    uint8_t interval;
    uint16_t max_packet;
} xhci_hid_candidate_t;

static int xhci_parse_hid(const uint8_t *config, size_t length,
                          xhci_hid_candidate_t *candidates, unsigned max,
                          uint8_t *configuration, int allow_vbox_tablet) {
    if (length < 9 || config[1] != USB_DT_CONFIG)
        return -EINVAL;
    *configuration = config[5];
    int current = -1;
    unsigned count = 0;
    for (size_t offset = 0; offset + 2 <= length;) {
        uint8_t size = config[offset];
        uint8_t type = config[offset + 1];
        if (size < 2 || offset + size > length)
            break;
        if (type == USB_DT_INTERFACE && size >= 9) {
            current = -1;
            int boot_hid = config[offset + 6] == 1 &&
                           (config[offset + 7] == 1 || config[offset + 7] == 2);
            int vbox_tablet = allow_vbox_tablet && config[offset + 6] == 0 &&
                              config[offset + 7] == 0;
            if (config[offset + 5] == USB_CLASS_HID &&
                (boot_hid || vbox_tablet) && count < max) {
                current = (int)count++;
                candidates[current].interface_number = config[offset + 2];
                candidates[current].protocol = boot_hid ? config[offset + 7] : 3;
            }
        } else if (type == USB_DT_ENDPOINT && size >= 7 && current >= 0 &&
                   (config[offset + 2] & USB_DIR_IN) &&
                   (config[offset + 3] & 3U) == 3U) {
            candidates[current].endpoint = config[offset + 2];
            candidates[current].max_packet =
                (uint16_t)config[offset + 4] | ((uint16_t)config[offset + 5] << 8);
            candidates[current].interval = config[offset + 6];
        }
        offset += size;
    }
    unsigned valid = 0;
    for (unsigned i = 0; i < count; i++)
        if (candidates[i].endpoint && candidates[i].max_packet)
            candidates[valid++] = candidates[i];
    return (int)valid;
}

static int xhci_configure_hid(xhci_controller_t *xhci, uint8_t slot,
                              uint8_t speed, const xhci_hid_candidate_t *list,
                              unsigned count) {
    if (!count || xhci->hid_count + count > XHCI_MAX_HID_ENDPOINTS)
        return -EINVAL;
    memset(xhci->input_context, 0, sizeof(xhci->input_context));
    arch_dma_sync_for_cpu(xhci->output_context[slot],
                          sizeof(xhci->output_context[slot]));
    uint32_t *control = xhci_input_context(xhci, 0);
    uint32_t *out_slot = xhci_output_context(xhci, slot, 0);
    uint32_t *in_slot = xhci_input_context(xhci, 1);
    memcpy(in_slot, out_slot, xhci->context_size);
    control[1] = 1U;
    uint8_t max_dci = 1;
    unsigned first_hid = xhci->hid_count;
    for (unsigned i = 0; i < count; i++) {
        uint8_t endpoint_number = list[i].endpoint & 0x0fU;
        uint8_t dci = (uint8_t)(endpoint_number * 2U + 1U);
        if (!endpoint_number || dci >= 32)
            return -EINVAL;
        xhci_hid_endpoint_t *hid = &xhci->hid[xhci->hid_count++];
        memset(hid, 0, sizeof(*hid));
        xhci_ring_init(&hid->ring);
        hid->slot_id = slot;
        hid->dci = dci;
        hid->interface_number = list[i].interface_number;
        hid->protocol = list[i].protocol;
        hid->report_size = list[i].protocol == 2 ? 4 : 8;
        if (hid->report_size > list[i].max_packet)
            hid->report_size = (uint8_t)list[i].max_packet;
        hid->valid = 1;
        control[1] |= 1U << dci;
        xhci_fill_ep_context(xhci, dci, &hid->ring, 7,
                             list[i].max_packet & 0x7ffU,
                             xhci_interval(speed, list[i].interval));
        if (dci > max_dci)
            max_dci = dci;
    }
    in_slot[0] = (in_slot[0] & ~(0x1fU << 27)) | ((uint32_t)max_dci << 27);
    arch_dma_sync_for_device(xhci->input_context, sizeof(xhci->input_context));
    int result = xhci_command(xhci, va_to_pa(xhci->input_context), 0,
                              XHCI_TRB_TYPE(XHCI_TRB_CONFIGURE_ENDPOINT) |
                              ((uint32_t)slot << 24), NULL);
    if (result) {
        xhci->hid_count = (uint8_t)first_hid;
        return result;
    }
    return 0;
}

static int xhci_enumerate_port(xhci_controller_t *xhci, unsigned port) {
    uint8_t speed;
    int result = xhci_reset_port(xhci, port, &speed);
    if (result) {
        kerr("[XHCI] port %u reset failed: %d\n", port, result);
        return result;
    }
    /* USB 2.0 reset recovery is at least 10 ms.  VBox completes PORTSC reset
     * before its emulated HID device is ready for the first control request. */
    mdelay(20);
    kinfo("[XHCI] port %u enabled: speed=%u\n", port, speed);
    uint8_t slot;
    result = xhci_enable_slot(xhci, &slot);
    if (result || slot > xhci->max_slots) {
        kerr("[XHCI] port %u enable-slot failed: result=%d slot=%u\n",
             port, result, result ? 0U : slot);
        return result ? result : -EINVAL;
    }
    kinfo("[XHCI] port %u assigned slot %u\n", port, slot);
    result = xhci_address_device(xhci, slot, (uint8_t)port, speed);
    if (result) {
        kerr("[XHCI] port %u address failed: %d\n", port, result);
        return result;
    }
    mdelay(5);
    result = xhci_get_descriptor(xhci, slot, USB_DT_DEVICE, 8);
    if (result) {
        kerr("[XHCI] port %u device descriptor(8) failed: %d\n", port,
             result);
        return result;
    }
    usb_device_descriptor_t *device = (usb_device_descriptor_t *)xhci->control_buffer;
    uint16_t max_packet = xhci_mps_value(speed, device->max_packet0);
    uint16_t initial_mps = xhci_mps_value(speed, xhci_default_mps(speed));
    if (max_packet && max_packet != initial_mps) {
        result = xhci_update_ep0_mps(xhci, slot, max_packet);
        if (result) {
            kerr("[XHCI] port %u EP0 MPS update failed: %d\n", port,
                 result);
            return result;
        }
    }
    result = xhci_get_descriptor(xhci, slot, USB_DT_DEVICE,
                                 sizeof(usb_device_descriptor_t));
    if (result) {
        kerr("[XHCI] port %u device descriptor failed: %d\n", port,
             result);
        return result;
    }
    device = (usb_device_descriptor_t *)xhci->control_buffer;
    uint16_t vendor = device->vendor;
    uint16_t product = device->product;

    result = xhci_get_descriptor(xhci, slot, USB_DT_CONFIG, 9);
    if (result) {
        kerr("[XHCI] port %u config descriptor(9) failed: %d\n", port,
             result);
        return result;
    }
    uint16_t total = (uint16_t)xhci->control_buffer[2] |
                     ((uint16_t)xhci->control_buffer[3] << 8);
    if (total < 9 || total > sizeof(xhci->control_buffer)) {
        kerr("[XHCI] port %u invalid config length: %u\n", port, total);
        return -EINVAL;
    }
    result = xhci_get_descriptor(xhci, slot, USB_DT_CONFIG, total);
    if (result) {
        kerr("[XHCI] port %u config descriptor failed: %d\n", port,
             result);
        return result;
    }
    xhci_hid_candidate_t candidates[XHCI_MAX_HID_ENDPOINTS];
    memset(candidates, 0, sizeof(candidates));
    uint8_t configuration = 0;
    int hid_count = xhci_parse_hid(xhci->control_buffer, total, candidates,
                                   ARRAY_SIZE(candidates), &configuration,
                                   vendor == VBOX_USB_VENDOR &&
                                   product == VBOX_USB_TABLET);
    if (hid_count <= 0) {
        kinfo("[XHCI] USB %04x:%04x on port %u has no boot HID interface\n",
              vendor, product, port);
        return 0;
    }
    result = xhci_configure_hid(xhci, slot, speed, candidates,
                                (unsigned)hid_count);
    if (result) {
        kerr("[XHCI] port %u endpoint configuration failed: %d\n", port,
             result);
        return result;
    }
    result = xhci_no_data_request(xhci, slot,
                                  USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                                  USB_REQ_SET_CONFIGURATION, configuration, 0);
    if (result) {
        kerr("[XHCI] port %u SET_CONFIGURATION failed: %d\n", port,
             result);
        return result;
    }
    unsigned first = xhci->hid_count - (unsigned)hid_count;
    for (unsigned i = first; i < xhci->hid_count; i++) {
        xhci_hid_endpoint_t *hid = &xhci->hid[i];
        if (hid->protocol == 1 || hid->protocol == 2) {
            result = xhci_no_data_request(xhci, slot,
                                          USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                                          USB_REQ_SET_PROTOCOL, 0,
                                          hid->interface_number);
            if (result)
                kerr("[XHCI] HID SET_PROTOCOL failed: slot=%u if=%u result=%d\n",
                     slot, hid->interface_number, result);
        }
        (void)xhci_no_data_request(xhci, slot,
                                   USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                                   USB_REQ_SET_IDLE, 0,
                                   hid->interface_number);
        xhci_submit_interrupt(xhci, hid);
        const char *kind = hid->protocol == 1 ? "keyboard" :
                           hid->protocol == 2 ? "mouse" : "tablet";
        kinfo("[XHCI] USB HID %s ready: port=%u slot=%u ep=%u\n", kind,
              port, slot, hid->dci);
    }
    return hid_count;
}

static int xhci_take_ownership(xhci_controller_t *xhci) {
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
                    return 0;
                arch_cpu_relax();
            }
            return -ETIMEDOUT;
        }
        offset = next ? offset + next : 0;
    }
    return 0;
}

static int xhci_controller_init(xhci_controller_t *xhci) {
    if (xhci_take_ownership(xhci) != 0)
        kerr("[XHCI] firmware ownership handoff timed out\n");
    xhci_write32(xhci->op, XHCI_USBCMD,
                 xhci_read32(xhci->op, XHCI_USBCMD) & ~XHCI_CMD_RUN);
    if (xhci_wait32(xhci->op, XHCI_USBSTS, XHCI_STS_HALTED,
                    XHCI_STS_HALTED) != 0)
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
    xhci_write32(ir0, 0x00U, 0x1U); /* Polling: clear IP and leave IE disabled. */
    xhci_write32(ir0, 0x08U, 1U);
    xhci_write64(ir0, 0x10U, va_to_pa(&xhci->erst));
    xhci_write64(ir0, 0x18U, va_to_pa(xhci->event_ring));
    xhci_write32(xhci->op, XHCI_CONFIG, xhci->max_slots);
    xhci_write32(xhci->op, XHCI_USBCMD, XHCI_CMD_RUN);
    if (xhci_wait32(xhci->op, XHCI_USBSTS, XHCI_STS_HALTED, 0) != 0)
        return -ETIMEDOUT;
    xhci->running = 1;
    return 0;
}

static int xhci_hid_read(device_t *dev, void *buffer, size_t count) {
    xhci_controller_t *xhci = dev ? dev->drv_priv : NULL;
    if (!xhci || !xhci->running || !buffer || count < sizeof(struct input_event))
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&xhci->lock);
    xhci_poll_locked(xhci);
    size_t copied = 0;
    while (xhci->input_tail != xhci->input_head &&
           copied + sizeof(struct input_event) <= count) {
        *(struct input_event *)((uint8_t *)buffer + copied) =
            xhci->input[xhci->input_tail];
        xhci->input_tail = (uint16_t)((xhci->input_tail + 1U) % XHCI_INPUT_EVENTS);
        copied += sizeof(struct input_event);
    }
    spin_unlock_irqrestore(&xhci->lock, flags);
    return copied ? (int)copied : -EAGAIN;
}

static int xhci_hid_ioctl(device_t *dev, unsigned long request, void *argument) {
    (void)dev;
    (void)request;
    (void)argument;
    return -ENOSYS;
}

static int xhci_hid_poll(device_t *dev, short events) {
    (void)events;
    xhci_controller_t *xhci = dev ? dev->drv_priv : NULL;
    if (!xhci || !xhci->running)
        return 0;
    uint64_t flags = spin_lock_irqsave(&xhci->lock);
    xhci_poll_locked(xhci);
    int ready = xhci->input_head != xhci->input_tail;
    spin_unlock_irqrestore(&xhci->lock, flags);
    return ready;
}

static const input_dev_ops_t xhci_hid_ops = {
    .read = xhci_hid_read,
    .ioctl = xhci_hid_ioctl,
    .poll = xhci_hid_poll,
};

static int xhci_hid_probe(device_t *dev) {
    if (pci_enable_and_assign_bars(dev) != 0)
        return -ENODEV;
    resource_t *bar = pci_get_bar_resource(dev, 0);
    if (!bar || bar->end < bar->start || bar->end - bar->start + 1U < 0x1000U)
        return -ENODEV;
    xhci_controller_t *xhci = &g_xhci;
    memset(xhci, 0, sizeof(*xhci));
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
    if (!cap_length || !xhci->max_slots || !xhci->max_ports)
        return -ENODEV;
    kinfo("[XHCI] controller: MMIO=0x%lx slots=%u ports=%u context=%u\n",
          (unsigned long)xhci->cap, xhci->max_slots, xhci->max_ports,
          xhci->context_size);
    int result = xhci_controller_init(xhci);
    if (result) {
        kerr("[XHCI] controller initialization failed: %d\n", result);
        return result;
    }
    for (unsigned port = 1; port <= xhci->max_ports; port++) {
        uint32_t portsc = xhci_read32(xhci->op,
                                      XHCI_PORTSC_BASE +
                                      (port - 1U) * XHCI_PORTSC_STRIDE);
        if (portsc & XHCI_PORT_CCS) {
            result = xhci_enumerate_port(xhci, port);
            if (result < 0)
                kerr("[XHCI] port %u enumeration failed: %d\n", port, result);
        }
    }
    if (!xhci->hid_count) {
        kerr("[XHCI] controller ready, but no boot keyboard/mouse was found\n");
        return -ENODEV;
    }
    dev->drv_priv = xhci;
    kinfo("[XHCI] ready: ports=%u slots=%u context=%u HID=%u (polling)\n",
          xhci->max_ports, xhci->max_slots, xhci->context_size,
          xhci->hid_count);
    return 0;
}

static int xhci_hid_remove(device_t *dev) {
    xhci_controller_t *xhci = dev ? dev->drv_priv : NULL;
    if (!xhci)
        return 0;
    xhci_write32(xhci->op, XHCI_USBCMD,
                 xhci_read32(xhci->op, XHCI_USBCMD) & ~XHCI_CMD_RUN);
    xhci->running = 0;
    dev->drv_priv = NULL;
    memset(xhci, 0, sizeof(*xhci));
    return 0;
}

static const device_id_t xhci_hid_ids[] = {
    { .vendor = XHCI_VENDOR_INTEL, .device = XHCI_DEVICE_PANTHER_POINT,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static driver_t xhci_hid_driver = {
    .name = "xhci-hid",
    .id_table = xhci_hid_ids,
    .bus = NULL,
    .probe = xhci_hid_probe,
    .remove = xhci_hid_remove,
    .class_ops = &xhci_hid_ops,
    .class_type = DEV_CLASS_INPUT,
};

DRIVER_REGISTER(xhci_hid_driver);
