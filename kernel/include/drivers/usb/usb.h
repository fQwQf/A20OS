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

#define USB_REQ_CLEAR_FEATURE   1U
#define USB_REQ_SET_FEATURE     3U
#define USB_REQ_SET_ADDRESS     5U
#define USB_REQ_GET_DESCRIPTOR    6U
#define USB_REQ_SET_CONFIGURATION 9U
#define USB_REQ_SET_IDLE          10U
#define USB_REQ_SET_PROTOCOL      11U

#define USB_DT_DEVICE    1U
#define USB_DT_CONFIG    2U
#define USB_DT_STRING    3U
#define USB_DT_INTERFACE 4U
#define USB_DT_ENDPOINT  5U
#define USB_DT_HUB       0x29U

#define USB_CLASS_HID          3U
#define USB_CLASS_MASS_STORAGE 8U
#define USB_CLASS_HUB          9U

/* Hub class feature selectors.  The "C_PORT_*" values are change bits: they
 * are acknowledged with CLEAR_FEATURE, not set. */
#define USB_FEAT_C_PORT_ENABLE      0x00U
#define USB_FEAT_C_PORT_RESET       0x08U
#define USB_FEAT_C_PORT_CONNECTION  0x10U
#define USB_FEAT_PORT_RESET         0x14U

/* A device behind a hub has no address of its own until the hub assigns one,
 * so the controller has to be told which hub owns the port when it builds the
 * device's context. */
#define USB_HUB_NONE               0U
#define USB_HUB_MAX_PORTS          127U

#define USB_SUBCLASS_SCSI        0x06U
#define USB_PROTOCOL_BULK_ONLY   0x50U

#define USB_REQ_BULK_ONLY_RESET  0xFFU
#define USB_REQ_GET_MAX_LUN      0xFEU

#define USB_SPEED_LOW   1U
#define USB_SPEED_FULL  2U
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

/* The fixed part of a hub descriptor.  Two variable-length bitmap arrays
 * follow: DeviceRemovable[ceil(bNbrPorts/8)] then PortPwrCtrlMask[] of the
 * same length, so the descriptor's total length is
 * 7 + 2 * ((bNbrPorts + 7) / 8). */
typedef struct __attribute__((packed)) usb_hub_descriptor {
    uint8_t  length;
    uint8_t  type;
    uint8_t  hub_type;           /* bHubType */
    uint8_t  ports;              /* bNbrPorts */
    uint16_t characteristics;    /* wHubCharacteristics */
    uint8_t  pwr_on2pwr_good;    /* bPwrOn2PwrGood */
    uint8_t  hub_contr_current;   /* bHubHdrDecLat */
} usb_hub_descriptor_t;

#define USB_HUB_CHAR_PER_PORT_OC     0x0001U
#define USB_HUB_CHAR_NO_TT           0x0002U
#define USB_HUB_CHAR_IND_OVERCURRENT 0x0100U
#define USB_HUB_CHAR_TT_PER_8        0x0200U
/* Bit 2 of wHubCharacteristics: the hub itself runs at SuperSpeed or above,
 * so its downstream ports are SuperSpeed ports. */
#define USB_HUB_CHAR_SUPERSPEED      0x0004U

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
     * @hub_address names the hub the port belongs to, or USB_HUB_NONE for a
     * root port.  @address is the USB address the core reserved for the
     * device; an HCD that speaks SET_ADDRESS applies it and echoes it back
     * in @out->address, while one that addresses devices implicitly leaves
     * it zero. */
    int  (*alloc_slot)(struct usb_hcd *hcd, unsigned port, uint8_t speed,
                       uint8_t hub_address, uint8_t address, usb_slot_t *out);
    /* Update EP0 max packet size after reading the device descriptor. */
    int  (*update_ep0_mps)(struct usb_hcd *hcd, uint8_t slot,
                           uint16_t max_packet);
    /* Raw control transfer on a slot's EP0. */
    int  (*control)(struct usb_hcd *hcd, uint8_t slot,
                    const usb_setup_packet_t *setup, void *data);
    /* Fetch a descriptor into buf (bounce buffer sized >= len). */
    int  (*get_descriptor)(struct usb_hcd *hcd, uint8_t slot, uint8_t type,
                           uint16_t len, void *buf);
    /* Configure an endpoint (allocates its transfer ring). */
    int  (*configure_endpoint)(struct usb_hcd *hcd, struct usb_device *dev,
                               uint8_t addr, uint8_t ep_type,
                               uint16_t max_packet, uint8_t interval);
    /* Arm a periodic IN transfer; completion calls urb->complete(). */
    int  (*submit_interrupt)(struct usb_hcd *hcd, usb_urb_t *urb);
    /* Submit a bulk transfer; completion calls urb->complete(). */
    int  (*submit_bulk)(struct usb_hcd *hcd, usb_urb_t *urb);
    /* Tear down a slot on removal. */
    int  (*abort_slot)(struct usb_hcd *hcd, uint8_t slot);
} usb_hcd_ops_t;

typedef struct usb_hcd {
    const usb_hcd_ops_t *ops;
    struct device *hcd_dev;     /* host controller device_t */
    struct usb_hcd *parent_hcd; /* root controller behind a hub-provided bus */
    uint8_t hub_address;        /* USB_HUB_NONE for a root controller */
    unsigned max_ports;
    uint8_t *port_state;        /* per-port enumeration state */
    struct usb_device **port_devices; /* owned device for each physical port */
    void    *priv;              /* HCD private (xhci_controller_t *) */
} usb_hcd_t;

/* ------------------------------------------------------------------ */
/* Core API                                                            */
/* ------------------------------------------------------------------ */

void usb_core_init(void);

int usb_core_register_hcd(usb_hcd_t *hcd);
void usb_core_unregister_hcd(usb_hcd_t *hcd);

/* Bus address pool.  A root controller may address devices implicitly, but a
 * device behind a hub is addressed by the hub and must be unique across the
 * whole bus, so every such address comes from here.  Returns 1..127, or a
 * negative errno. */
int usb_core_alloc_address(void);
void usb_core_free_address(uint8_t address);

/* Enumerate connected ports of all registered HCDs.  Called after
 * driver_probe_all() so interface device_register() does not re-enter the
 * driver-core registration mutex. */
void usb_core_scan(void);

/* Process-context hotplug poll.  It is rate-limited internally and must not
 * be called from an interrupt handler because add/remove invokes drivers. */
void usb_core_poll(void);

/* Synchronously enumerate one port (called by the HCD at probe and, in a
 * later phase, on hotplug detection). */
int usb_core_enumerate_port(usb_hcd_t *hcd, unsigned port);

/* Generic control transfer on a device's EP0. */
int usb_control_msg(usb_device_t *dev, uint8_t request_type, uint8_t request,
                    uint16_t value, uint16_t index, void *data, uint16_t len);

int usb_submit_urb(usb_urb_t *urb);

/* Interface device helper: get the interface by its number. */
usb_interface_t *usb_find_interface(usb_device_t *dev, uint8_t num);

#endif /* _USB_USB_H */
