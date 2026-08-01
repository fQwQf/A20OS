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

#define USB_REQ_GET_DESCRIPTOR    6U
#define USB_REQ_SET_CONFIGURATION 9U
#define USB_REQ_SET_IDLE          10U
#define USB_REQ_SET_PROTOCOL      11U

#define USB_DT_DEVICE    1U
#define USB_DT_CONFIG    2U
#define USB_DT_STRING    3U
#define USB_DT_INTERFACE 4U
#define USB_DT_ENDPOINT  5U

#define USB_CLASS_HID          3U
#define USB_CLASS_MASS_STORAGE 8U
#define USB_CLASS_HUB          9U

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

/* ------------------------------------------------------------------ */
/* Device model                                                        */
/* ------------------------------------------------------------------ */

struct usb_device;
struct usb_interface;
struct usb_endpoint;
struct usb_hcd;

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
    /* Assign a slot and address the device at EP0. */
    int  (*init_slot)(struct usb_hcd *hcd, unsigned port, uint8_t speed,
                      uint8_t *slot);
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
    /* Tear down a slot on removal. */
    int  (*abort_slot)(struct usb_hcd *hcd, uint8_t slot);
} usb_hcd_ops_t;

typedef struct usb_hcd {
    const usb_hcd_ops_t *ops;
    struct device *hcd_dev;     /* host controller device_t */
    unsigned max_ports;
    uint8_t *port_state;        /* per-port enumeration state */
    void    *priv;              /* HCD private (xhci_controller_t *) */
} usb_hcd_t;

/* ------------------------------------------------------------------ */
/* Core API                                                            */
/* ------------------------------------------------------------------ */

void usb_core_init(void);

int usb_core_register_hcd(usb_hcd_t *hcd);
void usb_core_unregister_hcd(usb_hcd_t *hcd);

/* Enumerate connected ports of all registered HCDs.  Called after
 * driver_probe_all() so interface device_register() does not re-enter the
 * driver-core registration mutex. */
void usb_core_scan(void);

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
