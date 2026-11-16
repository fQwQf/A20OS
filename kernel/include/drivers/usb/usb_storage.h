#ifndef _DRIVERS_USB_USB_STORAGE_H
#define _DRIVERS_USB_USB_STORAGE_H

#include "drivers/block/block_dev.h"

/* Number of USB mass-storage disks we can expose. */
#define USB_STORAGE_MAX_DEVS 4

/* Each successful getter owns one consumer reference. Keep it across all
 * cache/I/O uses; after destroying borrowed caches, call dev->release(dev).
 * Old references remain a closed identity across unbind, never a new disk. */
block_dev_t *usb_storage_get_dev(int index);
int usb_storage_ready(int index);

#endif /* _DRIVERS_USB_USB_STORAGE_H */
