/*
 * MCU trim of the driver-class publication layer.
 *
 * The MCU profile has no devfs/sysfs/netlink, so class devices are not
 * published.  Platform drivers still go through the standard driver core;
 * this file supplies the class hooks it calls so the same driver_t/probe
 * lifecycle is used on the MCU.
 */
#ifdef CONFIG_MCU
#include "drivers/core/driver_class.h"

int class_device_publish(struct device *dev)
{
    (void)dev;
    return 0;
}

void class_device_unpublish(struct device *dev)
{
    (void)dev;
}

void class_device_mark_offline(struct device *dev)
{
    (void)dev;
}

class_device_t *class_device_get_by_type(uint32_t class_type, unsigned index)
{
    (void)class_type;
    (void)index;
    return NULL;
}

class_device_t *class_device_get_by_index(uint32_t class_type, unsigned index)
{
    (void)class_type;
    (void)index;
    return NULL;
}

class_device_t *class_device_ref_for_device(const struct device *dev)
{
    (void)dev;
    return NULL;
}

void class_device_get(class_device_t *cdev)
{
    (void)cdev;
}

void class_device_put(class_device_t *cdev)
{
    (void)cdev;
}

int class_device_call_begin(class_device_t *cdev)
{
    (void)cdev;
    return -1;
}

void class_device_call_end(class_device_t *cdev)
{
    (void)cdev;
}
#endif
