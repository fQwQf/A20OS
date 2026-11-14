#include "drivers/core/driver_class.h"
#include "drivers/core/driver_protocol_internal.h"
#include "drivers/core/driver_core.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"
#include "mm/slab.h"
#include "net/socket/socket_internal.h"

/* class_allocate_index() hands out a unique (class_type, index) pair per
 * device in 0..255, so the total number of published class devices is bounded
 * by 256 per class type. Block indices are a lifetime budget and never reused;
 * other classes can reuse an index after its publication is removed.
 * Backing the registry with a fixed
 * static array keeps kcalloc/krealloc out of g_class_lock entirely: both
 * allocate through the reclaiming allocator, whose OOM path takes tasklist_lock,
 * a task park_lock and mm->lock, writes swap pages to disk, waits on remote TLB
 * IPIs and can
 * kerr()/proc_force_exit() a task, none of which may run with interrupts
 * disabled.  The bound is exact, not a guess: an index is never reused while
 * its device is published. */
#define CLASS_INDEX_SPACE     256U
#define CLASS_DEVICE_MAX      ((DEV_CLASS_AUDIO + 1) * CLASS_INDEX_SPACE)

static class_device_t *g_class_devices[CLASS_DEVICE_MAX];
static unsigned g_class_count;
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

/* /sys/class/<subsystem> name for a device class (used by udev uevents). */
const char *class_device_subsystem(uint32_t type)
{
    switch (type) {
    case DEV_CLASS_CHAR: return "char";
    case DEV_CLASS_BLOCK: return "block";
    case DEV_CLASS_NET: return "net";
    case DEV_CLASS_INPUT: return "input";
    case DEV_CLASS_DISPLAY: return "display";
    case DEV_CLASS_AUDIO: return "audio";
    default: return NULL;
    }
}

/*
 * Replay a uevent for every currently published class device.  Called by the
 * netlink layer when udevadm trigger asks for a fresh enumeration, so udevd
 * sees the same device set devfs/sysfs already expose.
 */
void class_device_emit_uevents(const char *action)
{
    if (!action)
        return;
    for (uint32_t type = 1; type <= DEV_CLASS_AUDIO; type++) {
        const char *subsystem = class_device_subsystem(type);
        if (!subsystem)
            continue;
        for (unsigned index = 0; index < 256; index++) {
            class_device_t *cdev = class_device_get_by_index(type, index);
            if (!cdev)
                continue;
            netlink_uevent_emit(action, subsystem, cdev->name, cdev->devt);
            class_device_put(cdev);
        }
    }
}

static uint64_t class_devt(uint32_t type, uint32_t index)
{
    static const uint16_t majors[] = { 0, 240, 8, 0, 13, 29, 116 };
    /* The input class device must report the devt of the devfs evdev node it
     * backs (/dev/input/eventN is 29:N+1), otherwise udev/libinput cannot
     * associate the /sys device with the device node. */
    if (type == DEV_CLASS_INPUT)
        return 0x1d01 + (index & 0xffU);
    return ((uint64_t)majors[type] << 8) | (index & 0xffU);
}

static int class_allocate_index(uint32_t type, uint32_t *index)
{
    /* Filesystems and UXFS retain numeric disk identities. A removed disk
     * index is never reassigned within this boot, including after wraparound. */
    if (type == DEV_CLASS_BLOCK) {
        if (g_class_next_index[type] >= CLASS_INDEX_SPACE) return -ENOSPC;
        *index = g_class_next_index[type];
        return 0;
    }
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
    if (device_is_stopping(dev)) return -ESHUTDOWN;
    class_device_t *cdev;
    int ret = driver_publication_create(dev, &cdev);
    if (ret || !cdev) return ret;
    if (cdev->class_type == DEV_CLASS_NONE) {
        uint64_t flags = spin_lock_irqsave(&g_class_lock);
        __atomic_store_n(&cdev->online, 1, __ATOMIC_RELEASE);
        dev->class_dev = cdev;
        spin_unlock_irqrestore(&g_class_lock, flags);
        return 0; /* Native protocols need neither a legacy class nor devfs. */
    }
    uint64_t flags = spin_lock_irqsave(&g_class_lock);
    if (g_class_count >= CLASS_DEVICE_MAX) {
        spin_unlock_irqrestore(&g_class_lock, flags);
        driver_publication_discard(dev, cdev);
        return -ENOSPC;
    }
    ret = class_allocate_index(cdev->class_type, &cdev->index);
    if (ret < 0) {
        spin_unlock_irqrestore(&g_class_lock, flags);
        driver_publication_discard(dev, cdev);
        return ret;
    }
    cdev->devt = class_devt(cdev->class_type, cdev->index);
    /* A driver may pick the node name from probe() (device_set_devfs_name);
     * everything else gets the generated "<prefix><index>". */
    if (dev->devfs_name[0])
        snprintf(cdev->name, sizeof(cdev->name), "%s", dev->devfs_name);
    else
        snprintf(cdev->name, sizeof(cdev->name), "%s%u",
                 class_prefix(cdev->class_type), cdev->index);
    /* Name resolution is first-match by name (devfs, sysfs, netlink), so a name
     * that is already published would give this device a node nothing can reach
     * and can shadow an unrelated one -- a driver-chosen name colliding with
     * another driver's, or claiming a generated "char3" for itself.  The index
     * and devt were unique; the name has to be too, or the registry holds two
     * devices answering to one name.  Refusing here makes the second probe
     * decline: driver_probe_bound_device() unwinds it like any other publish
     * failure.  This also covers a driver-chosen name against a generated one
     * in either order, since it walks every published device. */
    for (unsigned i = 0; i < g_class_count; i++) {
        if (strcmp(g_class_devices[i]->name, cdev->name) == 0) {
            spin_unlock_irqrestore(&g_class_lock, flags);
            driver_publication_discard(dev, cdev);
            return -EEXIST;
        }
    }
    if (cdev->class_type == DEV_CLASS_BLOCK)
        g_class_next_index[DEV_CLASS_BLOCK] = cdev->index + 1;
    __atomic_store_n(&cdev->online, 1, __ATOMIC_RELEASE);
    g_class_devices[g_class_count++] = cdev;
    dev->class_dev = cdev;
    spin_unlock_irqrestore(&g_class_lock, flags);

    /* Notify udev listeners (NETLINK_KOBJECT_UEVENT) that the device exists;
     * udevd uses this to run its rules and create /dev symlinks/permissions. */
    const char *subsystem = class_device_subsystem(cdev->class_type);
    if (subsystem)
        netlink_uevent_emit("add", subsystem, cdev->name, cdev->devt);
    return 0;
}

void class_device_unpublish(struct device *dev)
{
    class_device_t *cdev = dev ? dev->class_dev : NULL;
    if (!cdev)
        return;
    class_device_mark_offline(dev);
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

/* Reading dev->class_dev on its own races class_device_unpublish(), which
 * clears the field and then drops the last reference: a caller that read the
 * pointer just before the clear can be left holding freed memory.  Taking the
 * reference under g_class_lock makes the load and the refcount bump a single
 * event relative to the clear, so a non-NULL return is always a live cdev and
 * unpublish's own put cannot run ahead of this one.  Callers that want to keep
 * a class device past the call should use class_device_get_by_*() instead. */
class_device_t *class_device_ref_for_device(const struct device *dev)
{
    if (!dev)
        return NULL;
    uint64_t flags = spin_lock_irqsave(&g_class_lock);
    class_device_t *cdev = dev->class_dev;
    class_device_get(cdev);
    spin_unlock_irqrestore(&g_class_lock, flags);
    return cdev;
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
        /* Rank lookups enumerate online publications densely. An offline
         * entry may remain registered while quiesce/drain is in progress;
         * it must not look like end-of-list to consumers. Stable publication
         * indices are handled separately by class_device_get_by_index(). */
        if (!match || !__atomic_load_n(&candidate->online, __ATOMIC_ACQUIRE))
            continue;
        if (found++ == nth) {
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
class_device_t *class_device_get_by_index(uint32_t type, unsigned index)
{
    class_device_t *result = NULL;
    uint64_t flags = spin_lock_irqsave(&g_class_lock);
    for (unsigned i = 0; i < g_class_count; i++) {
        class_device_t *cdev = g_class_devices[i];
        if (cdev->class_type == type && cdev->index == index &&
            __atomic_load_n(&cdev->online, __ATOMIC_ACQUIRE)) {
            class_device_get(cdev); result = cdev; break;
        }
    }
    spin_unlock_irqrestore(&g_class_lock, flags);
    return result;
}
class_device_t *class_device_get_nth(unsigned index)
{
    return class_device_get_match(NULL, DEV_CLASS_NONE, index, 0);
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

int class_net_ready(class_device_t *cdev)
{
    const void *table;
    if (class_device_ops_begin(cdev, DEV_CLASS_NET, &table)) return 0;
    const net_dev_ops_t *ops = table;
    int ready = ops->send && ops->recv && ops->mac;
    class_device_call_end(cdev);
    return ready;
}
#define NET_OPERATION(name, arguments, expression) \
int name arguments { \
    const void *table; \
    int rc = class_device_ops_begin(cdev, DEV_CLASS_NET, &table); \
    if (rc) return rc; \
    const net_dev_ops_t *ops = table; \
    rc = (expression); \
    class_device_call_end(cdev); \
    return rc; \
}
NET_OPERATION(class_net_send, (class_device_t *cdev, const void *packet, size_t length),
              ops->send ? ops->send(cdev->dev, packet, length) : -EOPNOTSUPP)
NET_OPERATION(class_net_recv, (class_device_t *cdev, void *packet, size_t length),
              ops->recv ? ops->recv(cdev->dev, packet, length) : -EOPNOTSUPP)
NET_OPERATION(class_net_send_sg, (class_device_t *cdev, const net_iovec_t *iov, unsigned count),
              ops->send_sg ? ops->send_sg(cdev->dev, iov, count) : -EOPNOTSUPP)
NET_OPERATION(class_net_link_up, (class_device_t *cdev),
              ops->link_up ? ops->link_up(cdev->dev) : 1)
NET_OPERATION(class_net_rx_irq_driven, (class_device_t *cdev),
              ops->rx_irq_driven ? ops->rx_irq_driven(cdev->dev) : 0)
#undef NET_OPERATION
void class_net_poll(class_device_t *cdev)
{
    const void *table;
    if (class_device_ops_begin(cdev, DEV_CLASS_NET, &table)) return;
    const net_dev_ops_t *ops = table;
    if (ops->poll) ops->poll(cdev->dev);
    class_device_call_end(cdev);
}
uint32_t class_net_caps(class_device_t *cdev)
{
    const void *table;
    if (class_device_ops_begin(cdev, DEV_CLASS_NET, &table)) return 0;
    const net_dev_ops_t *ops = table;
    uint32_t caps = ops->caps ? ops->caps(cdev->dev) : 0;
    if (!ops->send_sg) caps &= ~NET_DEV_CAP_TX_SG;
    class_device_call_end(cdev);
    return caps;
}
int class_net_copy_mac(class_device_t *cdev, uint8_t out[6])
{
    if (!out) return -EINVAL;
    const void *table;
    int rc = class_device_ops_begin(cdev, DEV_CLASS_NET, &table);
    if (rc) return rc;
    const net_dev_ops_t *ops = table;
    const uint8_t *mac = ops->mac ? ops->mac(cdev->dev) : NULL;
    if (mac) memcpy(out, mac, 6);
    class_device_call_end(cdev);
    return mac ? 0 : -EOPNOTSUPP;
}
