/* SPDX-License-Identifier: Apache-2.0 */
#include "drivers/core/driver_protocol_internal.h"
#include "drivers/core/driver_core.h"
#include "drivers/audio/audio_core.h"
#include "drivers/net/interface.h"
#include "core/errno.h"
#include "core/string.h"
#include "mm/slab.h"

static size_t class_ops_size(uint32_t type)
{
    switch (type) {
    case DEV_CLASS_CHAR: return sizeof(char_dev_ops_t);
    case DEV_CLASS_BLOCK: return sizeof(block_dev_ops_t);
    case DEV_CLASS_NET: return sizeof(net_dev_ops_t);
    case DEV_CLASS_INPUT: return sizeof(input_dev_ops_t);
    case DEV_CLASS_DISPLAY: return sizeof(gpu_dev_ops_t);
    case DEV_CLASS_AUDIO: return sizeof(audio_dev_ops_t);
    default: return 0;
    }
}
static void binding_release(void *opaque)
{
    struct class_publication *p = opaque;
    if (p->has_stop_contract && !__atomic_load_n(&p->public.online, __ATOMIC_ACQUIRE))
        __atomic_store_n(&p->binding_stopped, 1, __ATOMIC_RELEASE);
    class_device_put(&p->public);
}
int driver_publication_create(struct device *dev, class_device_t **out)
{
    *out = NULL;
    if (!dev || !dev->drv) return -EINVAL;
    if (dev->class_dev) return -EEXIST;
    uint32_t type = dev->drv->class_type;
    bool primary = type != DEV_CLASS_NONE && dev->drv->class_ops;
    if (type > DEV_CLASS_AUDIO) return -EINVAL;
    size_t added = device_protocols_copy(dev, NULL), count;
    if (__builtin_add_overflow(added, (size_t)primary, &count) ||
        count > (SIZE_MAX - sizeof(struct class_publication)) /
                    sizeof(struct driver_protocol_descriptor)) return -E2BIG;
    if (!count) return 0;
    struct class_publication *p = kcalloc(1, sizeof(*p) + count * sizeof(p->protocols[0]));
    if (!p) return -ENOMEM;
    class_device_t *cdev = &p->public;
    cdev->dev = device_get(dev);
    if (!cdev->dev) { kfree(p); return -ENODEV; }
    cdev->class_type = primary ? type : DEV_CLASS_NONE;
    refcount_set(&cdev->refs, 1);
    spin_init(&cdev->state_lock);
    p->has_stop_contract = dev->drv->remove != NULL;
    p->protocol_count = count;
    device_protocols_copy(dev, p->protocols);
    if (primary) p->protocols[added] = (struct driver_protocol_descriptor){
        .info = {driver_protocol_class_id(type), 1, class_ops_size(type), 0},
        .ops = dev->drv->class_ops,
    };
    for (size_t i = 1; i < count; i++) {
        struct driver_protocol_descriptor item = p->protocols[i];
        size_t j = i;
        while (j && p->protocols[j - 1].info.id > item.info.id) {
            p->protocols[j] = p->protocols[j - 1]; j--;
        }
        p->protocols[j] = item;
    }
    class_device_get(cdev);
    int ret = device_add_resource_action(dev, binding_release, p);
    if (ret) { class_device_put(cdev); class_device_put(cdev); return ret; }
    *out = cdev;
    return 0;
}
void driver_publication_discard(struct device *dev, class_device_t *cdev)
{
    (void)device_release_resource_action(dev, binding_release, cdev);
    class_device_put(cdev);
}
bool class_device_binding_stopped(const class_device_t *cdev)
{
    const struct class_publication *p = (const void *)cdev;
    return p && __atomic_load_n(&p->binding_stopped, __ATOMIC_ACQUIRE) != 0;
}
void class_device_get(class_device_t *cdev)
{ if (cdev) refcount_inc(&cdev->refs); }
void class_device_put(class_device_t *cdev)
{
    if (cdev && refcount_dec_and_test(&cdev->refs)) {
        struct device *dev = cdev->dev;
        kfree(cdev); device_put(dev);
    }
}
void class_device_mark_offline(struct device *dev)
{
    class_device_t *cdev = dev ? dev->class_dev : NULL;
    if (!cdev) return;
    uint64_t flags = spin_lock_irqsave(&cdev->state_lock);
    __atomic_store_n(&cdev->online, 0, __ATOMIC_RELEASE);
    void (*offline)(class_device_t *, void *) = cdev->offline;
    void *data = cdev->offline_data;
    cdev->offline = NULL; cdev->offline_data = NULL;
    spin_unlock_irqrestore(&cdev->state_lock, flags);
    if (offline) offline(cdev, data);
}
int class_device_set_offline(class_device_t *cdev,
                             void (*callback)(class_device_t *, void *), void *data)
{
    if (!cdev || !callback) return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&cdev->state_lock);
    int ret = 0;
    if (!__atomic_load_n(&cdev->online, __ATOMIC_ACQUIRE)) ret = -ENODEV;
    else if (cdev->offline && cdev->offline != callback) ret = -EBUSY;
    else { cdev->offline = callback; cdev->offline_data = data; }
    spin_unlock_irqrestore(&cdev->state_lock, flags);
    return ret;
}
int class_device_call_begin(class_device_t *cdev)
{
    if (!cdev) return -ENODEV;
    uint64_t flags = spin_lock_irqsave(&cdev->state_lock);
    if (!__atomic_load_n(&cdev->online, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&cdev->state_lock, flags); return -ENODEV;
    }
    __atomic_add_fetch(&cdev->active_calls, 1, __ATOMIC_ACQUIRE);
    spin_unlock_irqrestore(&cdev->state_lock, flags);
    return 0;
}
int class_device_ops_begin(class_device_t *cdev, uint32_t type, const void **ops)
{
    if (!ops) return -EINVAL;
    *ops = NULL;
    int ret = class_device_call_begin(cdev);
    if (ret) return ret;
    struct driver *drv = cdev->dev->drv;
    if (!drv || cdev->class_type != type || drv->class_type != type || !drv->class_ops) {
        class_device_call_end(cdev); return -ENODEV;
    }
    *ops = drv->class_ops;
    return 0;
}
void class_device_call_end(class_device_t *cdev)
{ if (cdev) __atomic_sub_fetch(&cdev->active_calls, 1, __ATOMIC_RELEASE); }

driver_endpoint_t *device_endpoint_get(const struct device *dev)
{ return class_device_ref_for_device(dev); }
void driver_endpoint_get(driver_endpoint_t *endpoint) { class_device_get(endpoint); }
void driver_endpoint_put(driver_endpoint_t *endpoint) { class_device_put(endpoint); }
size_t driver_endpoint_protocol_count(const driver_endpoint_t *endpoint)
{
    const struct class_publication *p = (const void *)endpoint;
    return p ? p->protocol_count : 0;
}
int driver_endpoint_protocol_info(const driver_endpoint_t *endpoint, size_t index,
                                    struct driver_protocol_info *out)
{
    if (!out) return -EINVAL;
    *out = (struct driver_protocol_info){0};
    const struct class_publication *p = (const void *)endpoint;
    if (!p || index >= p->protocol_count) return -EINVAL;
    *out = p->protocols[index].info;
    return 0;
}
int driver_protocol_begin(driver_endpoint_t *endpoint,
                             const struct driver_protocol_requirement *required,
                             struct driver_protocol_access *out)
{
    if (!out) return -EINVAL;
    *out = (struct driver_protocol_access){0};
    if (!endpoint || !required || !required->id || !required->version || !required->ops_size)
        return -EINVAL;
    int ret = class_device_call_begin(endpoint);
    if (ret) return ret;
    const struct class_publication *p = (const void *)endpoint;
    size_t first = 0, end = p->protocol_count;
    while (first < end) {
        size_t middle = first + (end - first) / 2;
        if (p->protocols[middle].info.id < required->id) first = middle + 1;
        else end = middle;
    }
    const struct driver_protocol_descriptor *entry = first < p->protocol_count ? &p->protocols[first] : NULL;
    ret = !entry || entry->info.id != required->id ? -EOPNOTSUPP :
          entry->info.version != required->version ? -EPROTONOSUPPORT :
          entry->info.ops_size < required->ops_size ||
          (entry->info.capabilities & required->capabilities) != required->capabilities ? -EOPNOTSUPP : 0;
    if (ret) { class_device_call_end(endpoint); return ret; }
    *out = (struct driver_protocol_access){endpoint, endpoint->dev, entry->ops, entry->info};
    return 0;
}
void driver_protocol_end(struct driver_protocol_access *access)
{
    if (!access) return;
    driver_endpoint_t *endpoint = access->endpoint;
    *access = (struct driver_protocol_access){0};
    if (endpoint) class_device_call_end(endpoint);
}

int class_net_read_stats(class_device_t *cdev, net_dev_stats_t *out)
{
    if (!out) return -EINVAL;
    *out = (net_dev_stats_t){0};
    if (!cdev || cdev->class_type != DEV_CLASS_NET) return -ENODEV;
    const struct driver_protocol_requirement required = {
        DRIVER_PROTOCOL_NET_STATS, 1, sizeof(struct driver_net_stats_ops), 0,
    };
    struct driver_protocol_access access;
    int ret = driver_protocol_begin(cdev, &required, &access);
    if (ret) return ret;
    const struct driver_net_stats_ops *ops = access.ops;
    ret = ops->read ? ops->read(access.device, out) : -EOPNOTSUPP;
    driver_protocol_end(&access);
    if (ret > 0) ret = -EIO;
    if (ret) *out = (net_dev_stats_t){0};
    return ret;
}
