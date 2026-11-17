/* Execute the whole production driver. Model MMIO, cache maintenance and the
 * already-tested core stop -> class drain -> remove -> devres ordering. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t paddr_t;
typedef struct { int held; } spinlock_t;
typedef struct { int held; } mutex_t;
static unsigned irq_depth;
static void spin_init(spinlock_t *l) { l->held = 0; }
static uint64_t spin_lock_irqsave(spinlock_t *l) {
    assert(!l->held && !irq_depth); l->held = 1; irq_depth++; return 0;
}
static int spin_trylock_irqsave(spinlock_t *l, uint64_t *flags) {
    if (l->held) return 0;
    *flags = spin_lock_irqsave(l); return 1;
}
static void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags) {
    (void)flags; assert(l->held && irq_depth == 1); l->held = 0; irq_depth--;
}
static void mutex_init(mutex_t *l) { l->held = 0; }
static int mutex_trylock(mutex_t *l) { if (l->held) return 0; l->held = 1; return 1; }
static void mutex_lock(mutex_t *l) { assert(mutex_trylock(l)); }
static void mutex_unlock(mutex_t *l) { assert(l->held); l->held = 0; }
#define ALIGNED(n) __attribute__((aligned(n)))
#define RES_MMIO 1
#define DEV_CLASS_NET 1
#define DMA_MASK_32BIT UINT64_C(0xffffffff)
#define DRIVER_REGISTER(driver)
typedef struct { uint64_t start, end; } resource_t;
typedef struct device {
    void *drv_priv;
    const char *name;
    resource_t resource;
    int managed, mapped, stopped;
    uint64_t dma_mask;
    int (*stop)(void *);
    void (*release)(void *);
    void *context;
} device_t;
typedef struct { const char *id; } firmware_device_id_t;
typedef struct {
    int (*send)(device_t *, const void *, size_t);
    int (*recv)(device_t *, void *, size_t);
    const uint8_t *(*mac)(device_t *);
    void (*poll)(device_t *);
    int (*link_up)(device_t *);
} net_dev_ops_t;
typedef struct {
    const char *name;
    const firmware_device_id_t *firmware_ids;
    int irq_optional;
    void *bus;
    int (*probe)(device_t *), (*remove)(device_t *);
    net_dev_ops_t *class_ops;
    int class_type;
} driver_t;
static int platform_bus;
static void host_log(const char *format, ...) { (void)format; }
#define kinfo host_log
static uint32_t readl(const volatile void *p);
static void writel(uint32_t v, volatile void *p);
static uint64_t timer_get_ticks(void);
static uint64_t clock_ticks_per_sec(void) { return 1000; }
static void mdelay(unsigned ms);
static paddr_t va_to_pa(const void *p);
static int dma_set_mask(device_t *dev, uint64_t mask);
static int dma_range_ok(const device_t *dev, uint64_t address, size_t size);
static void dma_sync_for_cpu(const void *p, size_t size);
static void dma_sync_for_device(const void *p, size_t size);
static resource_t *device_get_resource(device_t *dev, int type, int index) {
    assert(type == RES_MMIO && !index); return &dev->resource;
}
static int device_probe_use_remove(device_t *dev) { dev->managed = 1; return 0; }
static int device_add_execution_action(device_t *, int (*)(void *), void (*)(void *), void *);
static void *device_map_mmio(device_t *, int);
/* PRODUCTION */

#define BASE UINT64_C(0x40040000)
#define PHYSICAL UINT64_C(0x20000000)
static device_t dev;
static uint32_t regs[0x1020 / 4];
/* Distinct DMA view: sync touches only an aligned, isolated descriptor or
 * buffer. Device completion writes do not magically modify the CPU view. */
static uint8_t dma_view[sizeof(g_ls2k_gmac_insts)];
static uint16_t phy[32];
static uint64_t ticks;
static unsigned reads, writes, resets, releases, unmaps, reset_reads;
static int fail_action, fail_map, fail_dma_gate, high_dma, dead, no_phy;
static int deny_mask, deny_dma_stop, deny_mac_stop, stuck_process, stuck_reset;
static int refuse_start, cancel_mdio, freeze_time;
static ls2k_gmac_priv_t *ctx(void) { assert(dev.context); return dev.context; }

static size_t storage_offset(const void *p, size_t size) {
    uintptr_t a = (uintptr_t)p, start = (uintptr_t)g_ls2k_gmac_insts;
    assert(a >= start && a - start <= sizeof(dma_view));
    assert(size <= sizeof(dma_view) - (a - start)); return a - start;
}
static paddr_t va_to_pa(const void *p) {
    size_t offset = storage_offset(p, 1);
    /* Make the first ring straddle 4 GiB while its first byte still fits. */
    if (high_dma) return UINT64_C(0xfffffff0) + offset;
    return PHYSICAL + offset;
}
static void *dma_pointer(uint32_t physical) {
    assert(physical >= PHYSICAL && physical - PHYSICAL < sizeof(dma_view));
    return dma_view + (physical - PHYSICAL);
}
static int dma_set_mask(device_t *d, uint64_t mask) {
    assert(mask == DMA_MASK_32BIT);
    if (fail_dma_gate) return -EOPNOTSUPP;
    d->dma_mask = mask; return 0;
}
static int dma_range_ok(const device_t *d, uint64_t address, size_t size) {
    return size && address <= d->dma_mask && size - 1 <= d->dma_mask - address;
}
static void sync_alignment(const void *p, size_t size) {
    assert(!((uintptr_t)p & 63));
    /* The only partial spans are isolated packet buffers. */
    assert(size == 64 || size == 1024 || size <= 1536 || size == 16 * 1536);
}
static void dma_sync_for_cpu(const void *p, size_t size) {
    sync_alignment(p, size); memcpy((void *)p, dma_view + storage_offset(p, size), size);
}
static void dma_sync_for_device(const void *p, size_t size) {
    sync_alignment(p, size); memcpy(dma_view + storage_offset(p, size), p, size);
}
static uint64_t timer_get_ticks(void) {
    assert(!irq_depth); if (!freeze_time) ticks++; return ticks;
}
static void mdelay(unsigned ms) { assert(!irq_depth); ticks += ms; }
static unsigned reg_offset(const volatile void *p) {
    uintptr_t a = (uintptr_t)p;
    assert(dev.mapped && a >= BASE && a < BASE + sizeof(regs) && !(a & 3));
    return a - BASE;
}
static uint32_t readl(const volatile void *p) {
    unsigned off = reg_offset(p); reads++;
    assert(!irq_depth || off == DMA_STATUS);
    if (dead) return UINT32_MAX;
    if (off == DMA_BUS_MODE && (regs[off/4] & 1) && !stuck_reset && ++reset_reads >= 2)
        regs[off/4] &= ~1U;
    if (off == DMA_STATUS && stuck_process) return (1U << 20) | (1U << 17);
    if (off == MAC_MII_ADDR && cancel_mdio) {
        cancel_mdio = 0; assert(dev.stop(dev.context) == -EBUSY);
    }
    return regs[off/4];
}
static void writel(uint32_t value, volatile void *p) {
    unsigned off = reg_offset(p); writes++;
    assert(dev.managed && dev.stop && ctx()->hardware_owned);
    if (dead) return;
    if (off == DMA_INTR_ENABLE && deny_mask) { regs[off/4] = 1; return; }
    if (off == DMA_CONTROL && deny_dma_stop && !(value & ((1U<<13)|2U))) return;
    if (off == MAC_CONFIGURATION && deny_mac_stop && !(value & 12U)) return;
    if (off == DMA_CONTROL && refuse_start && (value & ((1U<<13)|2U))) return;
    if (off == DMA_STATUS) { regs[off/4] &= ~(value & 0x1ffff); return; }
    if (off == DMA_BUS_MODE && (value & 1)) {
        assert(!irq_depth && ctx()->control_lock.held);
        resets++; reset_reads = 0;
        regs[DMA_CONTROL/4] = regs[DMA_INTR_ENABLE/4] = 0;
        regs[DMA_TX_BASE_ADDR/4] = regs[DMA_RX_BASE_ADDR/4] = 0;
    }
    if (off == MAC_MII_ADDR && (value & 1)) {
        assert(!irq_depth && ctx()->control_lock.held);
        unsigned addr = (value >> 11) & 31, reg = (value >> 6) & 31;
        if (value & 2) phy[reg] = regs[MAC_MII_DATA/4] & ~0x8000U;
        else regs[MAC_MII_DATA/4] = addr == 1 && !no_phy ? phy[reg] : 0xffff;
        value &= ~1U;
    }
    regs[off/4] = value;
}
static int device_add_execution_action(device_t *d, int (*stop)(void *),
                                        void (*release)(void *), void *context) {
    assert(d->managed && !d->mapped && context == d->drv_priv);
    if (fail_action) return -ENOMEM;
    d->stop = stop; d->release = release; d->context = context; return 0;
}
static void *device_map_mmio(device_t *d, int index) {
    assert(!index && d->stop && d->context);
    if (fail_map) return NULL;
    d->mapped = 1; return (void *)(uintptr_t)BASE;
}
static void reset(void) {
    assert(!dev.context && !dev.drv_priv && !irq_depth);
    for (unsigned i = 0; i < GMAC_MAX_INSTANCES; i++) assert(!g_ls2k_gmac_insts[i].valid);
    memset(&dev, 0, sizeof(dev)); memset(regs, 0, sizeof(regs));
    memset(dma_view, 0, sizeof(dma_view)); memset(phy, 0, sizeof(phy));
    dev.name = "model"; dev.resource = (resource_t){BASE, BASE+sizeof(regs)-1};
    regs[MAC_VERSION/4] = 0xd137; phy[2] = 0x1234; phy[3] = 0x5678; phy[1] = 4;
    ticks = reads = writes = resets = releases = unmaps = reset_reads = 0;
    fail_action = fail_map = fail_dma_gate = high_dma = dead = no_phy = 0;
    deny_mask = deny_dma_stop = deny_mac_stop = stuck_process = stuck_reset = 0;
    refuse_start = cancel_mdio = freeze_time = 0;
}
static int unbind(void) {
    int ret;
    if (dev.stop && !dev.stopped) {
        ret = dev.stop(dev.context); if (ret) return ret; dev.stopped = 1;
    }
    ret = ls2k_gmac_driver.remove(&dev); if (ret) return ret;
    if (dev.mapped) { dev.mapped = 0; unmaps++; }
    if (dev.release) { dev.release(dev.context); releases++; }
    dev.context = NULL; dev.stop = NULL; dev.release = NULL; return 0;
}
static void retained(ls2k_gmac_priv_t *saved) {
    assert(dev.context == saved && dev.drv_priv == saved && saved->valid && saved->owner == &dev);
    assert(dev.mapped && !releases && !unmaps && !saved->stopped);
    /* Another reservation cannot reuse or zero the failed owner's bytes. */
    saved->tx_buf[0][0] = 0x5a;
    ls2k_gmac_priv_t *other = gmac_alloc_instance(&dev);
    assert(other && other != saved && saved->tx_buf[0][0] == 0x5a);
    ls2k_gmac_release(other);
    unsigned count = reads + writes; uint8_t packet[4096] = {0};
    assert(ls2k_gmac_class_send(&dev, packet, 64) < 0);
    assert(ls2k_gmac_class_recv(&dev, packet, sizeof(packet)) < 0);
    ls2k_gmac_class_poll(&dev);
    assert(!ls2k_gmac_class_link_up(&dev) && !ls2k_gmac_class_mac(&dev));
    assert(reads + writes == count);
}
static void receive(uint32_t status, int expected, size_t maxlen) {
    ls2k_gmac_priv_t *priv = ctx(); unsigned i = priv->rx_busy;
    gmac_desc_t *d = dma_pointer(va_to_pa(&priv->rx_desc[i]));
    uint8_t *packet = dma_pointer(d->buffer1); memset(packet, 0xa6, 1536);
    d->status = status;
    uint8_t output[4096]; memset(output, 0x55, sizeof(output));
    assert(ls2k_gmac_recv(BASE, output, maxlen) == expected);
    assert(output[expected > 0 ? (unsigned)expected : 0] == 0x55);
    for (int j = 0; j < expected; j++) assert(output[j] == 0xa6);
    assert(d->status == (1U<<31) && d->length == (1536U | (1U<<14)));
    assert(d->buffer2 == va_to_pa(&priv->rx_desc[(i+1)%16]));
}
int main(void) {
    reset(); dev.resource.end = BASE+3;
    assert(ls2k_gmac_driver.probe(&dev) == -ENODEV && !dev.context && !reads && !writes);
    reset(); fail_action = 1;
    assert(ls2k_gmac_driver.probe(&dev) == -ENOMEM && !dev.drv_priv && !dev.mapped); assert(!unbind());
    reset(); fail_map = 1;
    assert(ls2k_gmac_driver.probe(&dev) == -ENODEV); assert(!unbind() && releases == 1 && !reads && !writes);
    reset(); fail_dma_gate = 1;
    assert(ls2k_gmac_driver.probe(&dev) == -EOPNOTSUPP); assert(!unbind() && !reads && !writes);
    reset(); high_dma = 1;
    assert(ls2k_gmac_driver.probe(&dev) == -ERANGE); assert(!unbind() && !reads && !writes);
    reset(); regs[MAC_VERSION/4] = 0x40;
    assert(ls2k_gmac_driver.probe(&dev) == -ENODEV); assert(!unbind() && !writes);
    reset(); stuck_reset = 1;
    assert(ls2k_gmac_driver.probe(&dev) == -ETIMEDOUT);
    ls2k_gmac_priv_t *saved = ctx(); assert(unbind() == -ETIMEDOUT); retained(saved);
    stuck_reset = 0; assert(!unbind() && releases == 1 && unmaps == 1);
    reset(); no_phy = 1;
    assert(ls2k_gmac_driver.probe(&dev) < 0 && ctx()->hardware_owned); assert(!unbind());
    reset(); refuse_start = 1;
    assert(ls2k_gmac_driver.probe(&dev) == -EIO); assert(!unbind());

    reset(); assert(!ls2k_gmac_driver.probe(&dev));
    ls2k_gmac_priv_t *priv = ctx(); assert(ls2k_gmac_class_link_up(&dev));
    assert(ls2k_gmac_driver.remove(&dev) == -EBUSY);
    /* Follow DMA's actual linked addresses over two wraps. Check fixed wire
     * bit positions independently of production names. */
    uint32_t current = regs[DMA_TX_BASE_ADDR/4];
    uint8_t packet[80]; memset(packet, 0x42, sizeof(packet));
    for (unsigned i = 0; i < 35; i++) {
        gmac_desc_t *d = dma_pointer(current);
        assert(!(d->status & (1U<<31)));
        assert(ls2k_gmac_send(BASE, packet, i ? sizeof(packet) : 1) == (i ? 80 : 1));
        assert(d->status == ((1U<<31)|(1U<<29)|(1U<<28)|(1U<<20)));
        assert(d->length == (i ? 80U : 60U));
        uint8_t *wire = dma_pointer(d->buffer1); assert(wire[0] == 0x42);
        if (!i) for (unsigned j = 1; j < 60; j++) assert(!wire[j]);
        d->status &= ~(1U<<31); current = d->buffer2;
    }
    assert(current == va_to_pa(&priv->tx_desc[3]));
    for (unsigned i = 0; i < 35; i++) receive((84U<<16)|0x300U, 80, 4096);
    receive((84U<<16)|0x300U, 7, 7);
    receive((1536U<<16)|0x300U, 1532, 4096);
    receive((1537U<<16)|0x300U, -EIO, 4096);
    receive((0x3fffU<<16)|0x300U, -EIO, 4096);
    receive((3U<<16)|0x300U, -EIO, 4096);
    receive((84U<<16)|0x100U, -EIO, 4096);
    receive((84U<<16)|0x8300U, -EIO, 4096);
    phy[1] = 0xffff; ticks += 300; assert(!ls2k_gmac_poll(BASE));
    assert(!ls2k_gmac_class_link_up(&dev));
    assert(!unbind() && releases == 1 && unmaps == 1);

    reset(); assert(!ls2k_gmac_driver.probe(&dev)); saved = ctx();
    mutex_lock(&saved->control_lock); assert(unbind() == -EBUSY); retained(saved);
    mutex_unlock(&saved->control_lock);
    uint64_t flags = spin_lock_irqsave(&saved->lock); assert(unbind() == -EBUSY);
    spin_unlock_irqrestore(&saved->lock, flags); retained(saved);
    deny_mask = 1; assert(unbind() == -ETIMEDOUT); retained(saved); deny_mask = 0;
    deny_dma_stop = 1; assert(unbind() == -ETIMEDOUT); retained(saved); deny_dma_stop = 0;
    /* The preceding failed DMA-stop attempt already disabled the MAC. Restore
     * a live MAC in the model before independently exercising its refusal. */
    regs[MAC_CONFIGURATION/4] |= 12U;
    deny_mac_stop = 1; assert(unbind() == -ETIMEDOUT); retained(saved); deny_mac_stop = 0;
    unsigned old_resets = resets; stuck_process = 1;
    assert(unbind() == -ETIMEDOUT && resets == old_resets); retained(saved);
    stuck_process = 0; dead = 1; assert(unbind() == -ENODEV); retained(saved); dead = 0;
    stuck_reset = 1; assert(unbind() == -ETIMEDOUT); retained(saved);
    stuck_reset = 0; assert(!unbind() && releases == 1 && unmaps == 1);
    /* A stopped timer must still terminate the wait and retain the owner. */
    reset(); assert(!ls2k_gmac_driver.probe(&dev)); saved = ctx();
    freeze_time = stuck_process = 1; assert(unbind() == -ETIMEDOUT); retained(saved);
    freeze_time = stuck_process = 0; assert(!unbind());
    /* Stop racing MDIO returns busy, closes admission and makes the existing
     * control owner exit; retry can then finish without IRQ-off waiting. */
    reset(); assert(!ls2k_gmac_driver.probe(&dev)); saved = ctx();
    cancel_mdio = 1; ticks += 300; assert(!ls2k_gmac_poll(BASE));
    assert(saved->closing && !saved->control_lock.held); retained(saved); assert(!unbind());
    reset(); assert(!ls2k_gmac_driver.probe(&dev)); assert(!unbind());
    puts("LS2K_GMAC_LIFETIME_OK: chained cache-isolated packets, RX bounds, managed partial probe, retained stop and retry");
}
