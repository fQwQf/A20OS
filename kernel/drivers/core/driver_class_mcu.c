/* SPDX-License-Identifier: Apache-2.0 */
/* MCU publishes native binding endpoints, without a class registry or VFS. */
#ifdef CONFIG_MCU
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_protocol_internal.h"
#include "core/errno.h"

static spinlock_t endpoint_lock = SPINLOCK_INIT;
int class_device_publish(struct device *dev)
{
    if (device_is_stopping(dev)) return -ESHUTDOWN;
    class_device_t *cdev;
    int ret = driver_publication_create(dev, &cdev);
    if (ret || !cdev) return ret;
    uint64_t flags = spin_lock_irqsave(&endpoint_lock);
    __atomic_store_n(&cdev->online, 1, __ATOMIC_RELEASE);
    dev->class_dev = cdev;
    spin_unlock_irqrestore(&endpoint_lock, flags);
    return 0;
}
void class_device_unpublish(struct device *dev)
{
    class_device_t *cdev = dev ? dev->class_dev : NULL;
    if (!cdev) return;
    class_device_mark_offline(dev);
    uint64_t flags = spin_lock_irqsave(&endpoint_lock);
    dev->class_dev = NULL;
    spin_unlock_irqrestore(&endpoint_lock, flags);
    while (__atomic_load_n(&cdev->active_calls, __ATOMIC_ACQUIRE))
        __asm__ volatile("" ::: "memory");
    class_device_put(cdev);
}
class_device_t *class_device_ref_for_device(const struct device *dev)
{
    if (!dev) return NULL;
    uint64_t flags = spin_lock_irqsave(&endpoint_lock);
    class_device_t *cdev = dev->class_dev;
    class_device_get(cdev);
    spin_unlock_irqrestore(&endpoint_lock, flags);
    return cdev;
}
class_device_t *class_device_get_by_name(const char *name) { (void)name; return NULL; }
class_device_t *class_device_get_by_type(uint32_t type, unsigned index)
{ (void)type; (void)index; return NULL; }
class_device_t *class_device_get_by_index(uint32_t type, unsigned index)
{ (void)type; (void)index; return NULL; }
class_device_t *class_device_get_nth(unsigned index) { (void)index; return NULL; }
int class_device_has_devnode(const class_device_t *cdev) { (void)cdev; return 0; }
uint8_t class_device_dirent_type(const class_device_t *cdev) { (void)cdev; return 0; }
const char *class_device_subsystem(uint32_t type) { (void)type; return NULL; }
void class_device_emit_uevents(const char *action) { (void)action; }
#endif
