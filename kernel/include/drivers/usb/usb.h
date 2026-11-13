#ifndef _USB_USB_H
#define _USB_USB_H

#include "core/types.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "drivers/core/driver_core.h"

/* ------------------------------------------------------------------ */
/* USB standard constants                                              */
/* ------------------------------------------------------------------ */

#define USB_DIR_IN      0x80U
#define USB_DIR_OUT     0x00U
#define USB_TYPE_STANDARD 0x00U
#define USB_TYPE_CLASS    0x20U
#define USB_RECIP_DEVICE   0x00U
#define USB_RECIP_INTERFACE 0x01U
#define USB_RECIP_OTHER     0x03U

#define USB_REQ_GET_STATUS      0U
#define USB_REQ_CLEAR_FEATURE   1U
#define USB_REQ_SET_FEATURE     3U
#define USB_REQ_SET_ADDRESS     5U
#define USB_REQ_GET_DESCRIPTOR    6U
#define USB_REQ_SET_CONFIGURATION 9U
#define USB_REQ_SET_INTERFACE   11U
#define USB_REQ_SET_IDLE          10U
#define USB_REQ_SET_PROTOCOL      11U

#define USB_DT_DEVICE    1U
#define USB_DT_CONFIG    2U
#define USB_DT_STRING    3U
#define USB_DT_INTERFACE 4U
#define USB_DT_ENDPOINT  5U
#define USB_DT_HUB       0x29U
#define USB_DT_SS_HUB    0x2aU

/* Bounded EP0 request size shared by enumeration and the HCD bounce buffer. */
#define USB_CONTROL_MAX_LENGTH 4096U

#define USB_CLASS_HID          3U
#define USB_CLASS_MASS_STORAGE 8U
#define USB_CLASS_HUB          9U

/* Hub class feature selectors. */
#define USB_FEAT_PORT_RESET          4U
#define USB_FEAT_PORT_POWER          8U
#define USB_FEAT_C_PORT_CONNECTION  16U
#define USB_FEAT_C_PORT_ENABLE      17U
#define USB_FEAT_C_PORT_SUSPEND     18U
#define USB_FEAT_C_PORT_OVER_CURRENT 19U
#define USB_FEAT_C_PORT_RESET       20U
#define USB_FEAT_C_BH_PORT_RESET    29U
#define USB_FEAT_C_PORT_LINK_STATE  25U
#define USB_FEAT_C_PORT_CONFIG_ERROR 26U
#define USB_REQ_SET_HUB_DEPTH       12U

/* Root ports have no parent hub slot. */
#define USB_HUB_NONE               0U
#define USB_HUB_MAX_PORTS          127U

#define USB_SUBCLASS_SCSI        0x06U
#define USB_PROTOCOL_BULK_ONLY   0x50U

#define USB_REQ_BULK_ONLY_RESET  0xFFU
#define USB_REQ_GET_MAX_LUN      0xFEU

#define USB_SPEED_FULL  1U
#define USB_SPEED_LOW   2U
#define USB_SPEED_HIGH  3U
#define USB_SPEED_SUPER 4U

#define USB_XFER_CONTROL    0U
#define USB_XFER_ISOC       1U
#define USB_XFER_BULK       2U
#define USB_XFER_INTERRUPT  3U

/* ------------------------------------------------------------------ */
/* Descriptor structures                                               */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) usb_setup_packet {
    uint8_t  request_type;
    uint8_t  request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} usb_setup_packet_t;

typedef struct __attribute__((packed)) usb_device_descriptor {
    uint8_t  length;
    uint8_t  type;
    uint16_t usb;
    uint8_t  dev_class;
    uint8_t  dev_subclass;
    uint8_t  dev_protocol;
    uint8_t  max_packet0;
    uint16_t vendor;
    uint16_t product;
    uint16_t device;
    uint8_t  manufacturer;
    uint8_t  product_string;
    uint8_t  serial;
    uint8_t  configurations;
} usb_device_descriptor_t;

typedef struct __attribute__((packed)) usb_config_descriptor {
    uint8_t  length;
    uint8_t  type;
    uint16_t total_length;
    uint8_t  num_interfaces;
    uint8_t  config_value;
    uint8_t  config_string;
    uint8_t  attributes;
    uint8_t  max_power;
} usb_config_descriptor_t;

typedef struct __attribute__((packed)) usb_interface_descriptor {
    uint8_t  length;
    uint8_t  type;
    uint8_t  interface_number;
    uint8_t  alt_setting;
    uint8_t  num_endpoints;
    uint8_t  interface_class;
    uint8_t  interface_subclass;
    uint8_t  interface_protocol;
    uint8_t  interface_string;
} usb_interface_descriptor_t;

typedef struct __attribute__((packed)) usb_endpoint_descriptor {
    uint8_t  length;
    uint8_t  type;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_descriptor_t;

/* Common seven-byte header of USB 2 and SuperSpeed hub descriptors. */
typedef struct __attribute__((packed)) usb_hub_descriptor {
    uint8_t  length;
    uint8_t  type;
    uint8_t  ports;              /* bNbrPorts */
    uint16_t characteristics;    /* wHubCharacteristics */
    uint8_t  pwr_on2pwr_good;    /* bPwrOn2PwrGood */
    uint8_t  hub_contr_current;
} usb_hub_descriptor_t;

#define USB_HUB_CHAR_TTT_SHIFT        5U

/* ------------------------------------------------------------------ */
/* Device model                                                        */
/* ------------------------------------------------------------------ */

struct usb_device;
struct usb_interface;
struct usb_endpoint;
struct usb_hcd;

/* What an HCD hands back when the core asks it to set a device up.
 *
 * @hcd is the controller-private token the rest of the HCD ops are keyed on
 * (an xHCI slot id); it is NOT the USB bus address, because a device behind
 * a hub owns an address the hub assigned while its controller context is
 * still owned by the root controller.  @address is the USB address, zero
 * when the controller addresses devices implicitly (xHCI), @port the
 * physical port the device sits on. */
typedef struct usb_slot {
    uint8_t hcd;
    uint8_t address;
    uint8_t port;
} usb_slot_t;

typedef struct usb_endpoint {
    struct usb_interface *iface;
    uint8_t  addr;              /* bEndpointAddress (with direction bit) */
    uint8_t  attrs;             /* bmAttributes */
    uint16_t max_packet;
    uint8_t  interval;
} usb_endpoint_t;

typedef struct usb_interface {
    struct usb_device *dev;
    uint8_t  interface_number;
    uint8_t  alt_setting;
    uint8_t  interface_class;
    uint8_t  interface_subclass;
    uint8_t  interface_protocol;
    usb_endpoint_t *eps;
    uint8_t  ep_count;
    struct device *device;      /* published device_t on the usb bus */
} usb_interface_t;

typedef struct usb_device {
    struct usb_hcd *hcd;
    uint8_t  address;
    uint8_t  speed;
    uint16_t vendor;
    uint16_t product;
    uint8_t  config_value;
    uint8_t  slot;              /* xHCI slot id (HCD-specific) */
    usb_interface_t *ifaces;
    uint8_t  iface_count;
} usb_device_t;

/* ------------------------------------------------------------------ */
/* URB                                                                 */
/* ------------------------------------------------------------------ */

typedef struct usb_urb {
    struct usb_device *dev;
    usb_endpoint_t    *ep;
    uint8_t            transfer_type;
    uint8_t            direction;
    void              *buf;
    size_t             len;
    int                status;
    void             (*complete)(struct usb_urb *urb);
    void              *ctx;               /* class-driver context */
} usb_urb_t;

/* ------------------------------------------------------------------ */
/* HCD interface                                                       */
/* ------------------------------------------------------------------ */

typedef struct usb_hcd_ops {
    /* Bring the controller up (rings, contexts, event ring). */
    int  (*start)(struct usb_hcd *hcd);
    /* Drain events (called from read/poll and kthread contexts). */
    int  (*poll)(struct usb_hcd *hcd);
    /* Reset a port; returns device speed. */
    int  (*reset_port)(struct usb_hcd *hcd, unsigned port, uint8_t *speed);
    /* Report whether a port has a device connected. */
    int  (*port_connected)(struct usb_hcd *hcd, unsigned port);
    /* Build a controller context for the device on @port and report the
     * token the other ops are keyed on.
     *
     * @hub_slot is the parent hub's controller token, or USB_HUB_NONE for a
     * root port. @address is an optional reservation for HCDs that assign
     * addresses in software. @out->address reports the assigned address.
     * The caller zeroes out. Nonzero out->hcd transfers a slot cleanup token
     * even on error; retain its context until abort_slot returns 0. Address is
     * the software reservation still used, or 0/an implicit address if unused.
     * With no token, the HCD retains any uncertain controller state privately. */
    int  (*alloc_slot)(struct usb_hcd *hcd, unsigned port, uint8_t speed,
                       uint8_t hub_slot, uint8_t address, usb_slot_t *out);
    /* Update EP0 max packet size after reading the device descriptor. */
    int  (*update_ep0_mps)(struct usb_hcd *hcd, uint8_t slot,
                           uint16_t max_packet);
    /* Synchronous EP0 transfer. On any return the HCD must no longer access
     * the caller's setup/data storage (use retained bounce storage on timeout). */
    int  (*control)(struct usb_hcd *hcd, uint8_t slot,
                    const usb_setup_packet_t *setup, void *data);
    /* Synchronous descriptor fetch; the same caller-buffer rule applies. */
    int  (*get_descriptor)(struct usb_hcd *hcd, uint8_t slot, uint8_t type,
                           uint16_t len, void *buf);
    /* Configure an endpoint (allocates its transfer ring). The class first
     * initializes device-side toggle/sequence state, including on rebind. */
    int  (*configure_endpoint)(struct usb_hcd *hcd, struct usb_device *dev,
                               uint8_t addr, uint8_t ep_type,
                               uint16_t max_packet, uint8_t interval);
    /* Arm a periodic IN transfer; completion calls urb->complete(). */
    int  (*submit_interrupt)(struct usb_hcd *hcd, usb_urb_t *urb);
    /* Submit a bulk transfer; completion calls urb->complete(). */
    int  (*submit_bulk)(struct usb_hcd *hcd, usb_urb_t *urb);
    /* 0 confirms slot/endpoint callbacks are stopped. Failure leaves their
     * contexts/storage valid for retry. Core retains the partial/published port and
     * address, and does not free interfaces/endpoints until this succeeds. */
    int  (*abort_slot)(struct usb_hcd *hcd, uint8_t slot);
    /* Publish hub topology to the host controller before addressing children. */
    int  (*configure_hub)(struct usb_hcd *hcd, struct usb_device *dev,
                          uint8_t ports, uint8_t tt_think_time);
    /* Task/lifecycle context. Permanently close and release one non-EP0
     * endpoint. 0 confirms no DMA or callback can touch its URBs/buffers;
     * errors retain endpoint state/storage for explicit retry. Idempotent
     * after success; other endpoints and the slot remain usable. ABI 10. */
    int  (*release_endpoint)(struct usb_hcd *hcd, struct usb_device *dev,
                             uint8_t addr);
} usb_hcd_ops_t;

typedef struct usb_hcd {
    const usb_hcd_ops_t *ops;
    struct device *hcd_dev;     /* host controller device_t */
    struct usb_hcd *parent_hcd; /* root controller behind a hub-provided bus */
    uint8_t hub_slot;           /* USB_HUB_NONE for a root controller */
    unsigned max_ports;
    uint8_t *port_state;        /* core-private retry count and cleanup phases */
    struct usb_device **port_devices; /* owned device for each physical port */
    void    *priv;              /* HCD private (xhci_controller_t *) */
    uint8_t hub_depth;          /* zero on the root bus */
} usb_hcd_t;

/* ------------------------------------------------------------------ */
/* Core API                                                            */
/* ------------------------------------------------------------------ */

void usb_core_init(void);

/* Task-context topology APIs use the native lifecycle coordinator, without
 * suppressing probes. Same-HCD reentry -> -EBUSY; duplicate -> -EEXIST;
 * invalid/stopping ancestor -> -ENODEV. Caller keeps HCD storage valid. */
int usb_core_register_hcd(usb_hcd_t *hcd);
/* Failure retains registration and remaining ownership for retry; already
 * removed interfaces stay removed. Active descendants or registered child
 * HCDs keep their upstream transport alive (-EBUSY). */
int usb_core_unregister_hcd(usb_hcd_t *hcd);

/* Address reservations for controllers that assign addresses in software. */
int usb_core_alloc_address(void);
void usb_core_free_address(uint8_t address);

/* Enumerate under lifecycle coordination. Nested scans are skipped;
 * callbacks can publish/probe downstream buses in the same pass. */
void usb_core_scan(void);

/* Process-context hotplug poll.  It is rate-limited internally and must not
 * be called from an interrupt handler because add/remove invokes drivers. */
void usb_core_poll(void);
/* Caller pins interface/protocol storage (e.g. an active class call).
 * Nonblocking topology entry: -EAGAIN when lifecycle is busy, -ENODEV for
 * stale/stopping/pending-removal devices; no callback then. Not an IRQ API. */
int usb_device_poll(usb_device_t *dev);
/* Current interface remove callback, after class calls drain. Releases each
 * described non-EP0 endpoint, preserving remaining ownership on error.
 * Caller retains interface/protocol storage; task context, not an IRQ API. */
int usb_interface_release_endpoints(usb_interface_t *iface);

/* Task context, lifecycle-serialized. Cleanup failure retains slot/address
 * for scan/unregister retry. Occupied/pending port -> -EBUSY; stopping HCD
 * or ancestor -> -ENODEV. Another retiring port on the same physical
 * controller defers allocation with -EAGAIN, without charging scan retries. */
int usb_core_enumerate_port(usb_hcd_t *hcd, unsigned port);

/* Generic control transfer on a device's EP0. */
int usb_control_msg(usb_device_t *dev, uint8_t request_type, uint8_t request,
                    uint16_t value, uint16_t index, void *data, uint16_t len);

int usb_submit_urb(usb_urb_t *urb);

/* Interface device helper: get the interface by its number. */
usb_interface_t *usb_find_interface(usb_device_t *dev, uint8_t num);

#endif /* _USB_USB_H */
