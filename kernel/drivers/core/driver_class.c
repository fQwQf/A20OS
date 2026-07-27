#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"
#include "mm/slab.h"

#define CLASS_DEVICE_INITIAL_CAP 32

static class_device_t **g_class_devices;
static unsigned g_class_count;
static unsigned g_class_cap;
static unsigned g_class_next_index[DEV_CLASS_AUDIO + 1];
static spinlock_t g_class_lock = SPINLOCK_INIT;

static const char *class_prefix(uint32_t type)
{
    switch (type) {
    case DEV_CLASS_CHAR: return "char";
    case DEV_CLASS_BLOCK: return "disk";
    case DEV_CLASS_NET: return "net";
    case DEV_CLASS_INPUT: return "event";
    case DEV_CLASS_DISPLAY: return "fb";
    case DEV_CLASS_AUDIO: return "audio";
    default: return NULL;
    }
}

static uint64_t class_devt(uint32_t type, uint32_t index)
{
    static const uint16_t majors[] = { 0, 240, 8, 0, 13, 29, 116 };
    return ((uint64_t)majors[type] << 8) | (index & 0xffU);
}

static int class_allocate_index(uint32_t type, uint32_t *index)
{
    unsigned start = g_class_next_index[type] & 0xffU;
    for (unsigned offset = 0; offset <= 0xffU; offset++) {
        unsigned candidate = (start + offset) & 0xffU;
        int used = 0;
        for (unsigned i = 0; i < g_class_count; i++) {
            if (g_class_devices[i]->class_type == type &&
                g_class_devices[i]->index == candidate) {
                used = 1;
                break;
            }
        }
        if (!used) {
            *index = candidate;
            g_class_next_index[type] = (candidate + 1U) & 0xffU;
            return 0;
        }
    }
    return -ENOSPC;
}

int class_device_publish(struct device *dev)
{
    if (!dev || !dev->drv || !dev->drv->class_ops ||
        dev->drv->class_type == DEV_CLASS_NONE ||
        dev->drv->class_type > DEV_CLASS_AUDIO)
        return 0;
    if (dev->class_dev)
        return -EEXIST;

    class_device_t *cdev = kcalloc(1, sizeof(*cdev));
    if (!cdev)
        return -ENOMEM;
    cdev->dev = dev;
    cdev->class_type = dev->drv->class_type;
    refcount_set(&cdev->refs, 1);
    spin_init(&cdev->state_lock);

    uint64_t flags = spin_lock_irqsave(&g_class_lock);
    if (!g_class_devices) {
        g_class_devices = kcalloc(CLASS_DEVICE_INITIAL_CAP,
                                  sizeof(*g_class_devices));
        if (!g_class_devices) {
            spin_unlock_irqrestore(&g_class_lock, flags);
            kfree(cdev);
            return -ENOMEM;
        }
        g_class_cap = CLASS_DEVICE_INITIAL_CAP;
    }
    if (g_class_count == g_class_cap) {
        unsigned new_cap = g_class_cap * 2;
        class_device_t **new_devices = krealloc(
            g_class_devices, new_cap * sizeof(*new_devices));
        if (!new_devices) {
            spin_unlock_irqrestore(&g_class_lock, flags);
            kfree(cdev);
            return -ENOMEM;
        }
        g_class_devices = new_devices;
        g_class_cap = new_cap;
    }
    int ret = class_allocate_index(cdev->class_type, &cdev->index);
    if (ret < 0) {
        spin_unlock_irqrestore(&g_class_lock, flags);
        kfree(cdev);
        return ret;
    }
    cdev->devt = class_devt(cdev->class_type, cdev->index);
    snprintf(cdev->name, sizeof(cdev->name), "%s%u",
             class_prefix(cdev->class_type), cdev->index);
    __atomic_store_n(&cdev->online, 1, __ATOMIC_RELEASE);
    g_class_devices[g_class_count++] = cdev;
    dev->class_dev = cdev;
    spin_unlock_irqrestore(&g_class_lock, flags);
    return 0;
}

void class_device_unpublish(struct device *dev)
{
    class_device_t *cdev = dev ? dev->class_dev : NULL;
    if (!cdev)
        return;

    uint64_t state_flags = spin_lock_irqsave(&cdev->state_lock);
    __atomic_store_n(&cdev->online, 0, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&cdev->state_lock, state_flags);
    uint64_t flags = spin_lock_irqsave(&g_class_lock);
    for (unsigned i = 0; i < g_class_count; i++) {
        if (g_class_devices[i] == cdev) {
            g_class_devices[i] = g_class_devices[--g_class_count];
            break;
        }
    }
    dev->class_dev = NULL;
    spin_unlock_irqrestore(&g_class_lock, flags);

    while (__atomic_load_n(&cdev->active_calls, __ATOMIC_ACQUIRE) != 0)
        __asm__ volatile("" ::: "memory");
    class_device_put(cdev);
}

void class_device_get(class_device_t *cdev)
{
    if (cdev)
        refcount_inc(&cdev->refs);
}

void class_device_put(class_device_t *cdev)
{
    if (cdev && refcount_dec_and_test(&cdev->refs))
        kfree(cdev);
}

static class_device_t *class_device_get_match(const char *name,
                                               uint32_t type,
                                               unsigned nth,
                                               int use_name)
{
    class_device_t *result = NULL;
    unsigned found = 0;
    uint64_t flags = spin_lock_irqsave(&g_class_lock);
    for (unsigned i = 0; i < g_class_count; i++) {
        class_device_t *candidate = g_class_devices[i];
        int match = use_name ? strcmp(candidate->name, name) == 0 :
                               (type == DEV_CLASS_NONE ||
                                candidate->class_type == type);
        if (!match)
            continue;
        if (found++ == nth &&
            __atomic_load_n(&candidate->online, __ATOMIC_ACQUIRE)) {
            class_device_get(candidate);
            result = candidate;
            break;
        }
    }
    spin_unlock_irqrestore(&g_class_lock, flags);
    return result;
}

class_device_t *class_device_get_by_name(const char *name)
{
    return name ? class_device_get_match(name, 0, 0, 1) : NULL;
}

class_device_t *class_device_get_by_type(uint32_t class_type, unsigned index)
{
    return class_device_get_match(NULL, class_type, index, 0);
}

class_device_t *class_device_get_nth(unsigned index)
{
    return class_device_get_match(NULL, DEV_CLASS_NONE, index, 0);
}

int class_device_call_begin(class_device_t *cdev)
{
    if (!cdev)
        return -ENODEV;
    uint64_t flags = spin_lock_irqsave(&cdev->state_lock);
    if (!__atomic_load_n(&cdev->online, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&cdev->state_lock, flags);
        return -ENODEV;
    }
    __atomic_add_fetch(&cdev->active_calls, 1, __ATOMIC_ACQUIRE);
    spin_unlock_irqrestore(&cdev->state_lock, flags);
    return 0;
}

void class_device_call_end(class_device_t *cdev)
{
    if (cdev)
        __atomic_sub_fetch(&cdev->active_calls, 1, __ATOMIC_RELEASE);
}

int class_device_has_devnode(const class_device_t *cdev)
{
    return cdev && (cdev->class_type == DEV_CLASS_CHAR ||
                    cdev->class_type == DEV_CLASS_BLOCK ||
                    cdev->class_type == DEV_CLASS_AUDIO);
}

uint8_t class_device_dirent_type(const class_device_t *cdev)
{
    return cdev && cdev->class_type == DEV_CLASS_BLOCK ? 6 : 2;
}
