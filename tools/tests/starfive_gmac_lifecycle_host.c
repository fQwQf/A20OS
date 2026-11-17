/* Execute the whole production driver. Model MMIO, cache maintenance and the
 * already-tested core stop -> class drain -> remove -> devres ordering. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
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
static void udelay(unsigned us);
/* PRODUCTION */

#define BASE UINT64_C(0x16040000)
static device_t dev;
static uint32_t regs[0x1164 / 4];
static uint8_t dma_view[sizeof(g_gmac_insts)] ALIGNED(64);
static uint16_t phy[32];
static uint64_t ticks, physical;
static unsigned reads, writes, resets, releases, unmaps, reset_reads, tx_hw, packets;
static int fail_action, fail_map, fail_dma_gate, dead, no_phy, bad_link;
static int deny_mask, deny_mac_mask, deny_tx_stop, deny_rx_stop, deny_mac_stop;
static int stuck_process, stuck_reset, refuse_start, refuse_stride, refuse_eame;
static int cancel_mdio, freeze_time, hold_tx;
static starfive_gmac_priv_t *ctx(void) { assert(dev.context); return dev.context; }
static size_t storage_offset(const void *p, size_t size) {
    uintptr_t a = (uintptr_t)p, start = (uintptr_t)g_gmac_insts;
    assert(a >= start && a - start <= sizeof(dma_view));
    assert(size <= sizeof(dma_view) - (a - start)); return a - start;
}
static paddr_t va_to_pa(const void *p) { return physical + storage_offset(p, 1); }
static void *dma_pointer(uint64_t pa) {
    assert(pa >= physical && pa - physical < sizeof(dma_view));
    return dma_view + (pa - physical);
}
static int dma_set_mask(device_t *d, uint64_t mask) {
    if (fail_dma_gate) return -EOPNOTSUPP;
    d->dma_mask = mask; return 0;
}
static int dma_range_ok(const device_t *d, uint64_t address, size_t size) {
    return size && address <= d->dma_mask && size - 1 <= d->dma_mask - address;
}
static void sync_alignment(const void *p, size_t size) {
    assert(!((uintptr_t)p & 63));
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
static void udelay(unsigned us) { assert(!irq_depth); ticks += !!us; }
static unsigned reg_offset(const volatile void *p) {
    uintptr_t a = (uintptr_t)p;
    assert(dev.mapped && a >= BASE && a < BASE + sizeof(regs) && !(a & 3));
    return a - BASE;
}
/* Derive the DMA stride and upper addresses from registers, independently
 * of the production slot type. Tail is only the low half of that window. */
static uint64_t list_address(int rx) {
    unsigned low = rx ? 0x111c : 0x1114, high = rx ? 0x1118 : 0x1110;
    return ((uint64_t)regs[high/4] << 32) | regs[low/4];
}
static void transmit(void) {
    unsigned stride = 16 + ((regs[0x1100/4] >> 18) & 7) * 8;
    assert(stride == 64 && regs[0x112c/4] == 15);
    uint64_t first = list_address(0);
    uint64_t tail = (first & ~UINT64_C(0xffffffff)) | regs[0x1120/4];
    unsigned walked = 0;
    while (first + tx_hw * stride != tail) {
        assert(walked++ < 15);
        dma_desc_t *d = dma_pointer(first + tx_hw * stride);
        assert((d->des3 & 0xb0000000U) == 0xb0000000U);
        uint64_t buffer = ((uint64_t)d->des1 << 32) | d->des0;
        assert(buffer == va_to_pa(ctx()->tx_buf[tx_hw]));
        assert(d->des2 == (d->des3 & 0x7fffU));
        uint8_t *wire = dma_pointer(buffer); assert(wire[0] == 0x42);
        if (d->des2 == 60) for (unsigned j = 1; j < 60; j++) assert(!wire[j]);
        /* Model a timestamp/status writeback replacing the buffer words. */
        d->des0 = 0xbad0; d->des1 = 0xbad1; d->des2 = 0xbad2; d->des3 = 0;
        tx_hw = (tx_hw + 1) % 16; packets++;
    }
}
static uint32_t readl(const volatile void *p) {
    unsigned off = reg_offset(p); reads++;
    assert(!irq_depth || off == DMA_CH0_STATUS);
    if (dead) return UINT32_MAX;
    if (off == DMA_MODE && (regs[off/4] & 1) && !stuck_reset && ++reset_reads >= 2)
        regs[off/4] &= ~1U;
    if (off == DMA_DEBUG_STATUS0 && stuck_process) return 0x1100;
    if (off == MAC_MII_ADDR && cancel_mdio) {
        cancel_mdio = 0; assert(dev.stop(dev.context) == -EBUSY);
    }
    return regs[off/4];
}
static void writel(uint32_t value, volatile void *p) {
    unsigned off = reg_offset(p); writes++;
    assert(dev.managed && dev.stop && ctx()->hardware_owned);
    if (dead) return;
    if (off == DMA_CH0_INTERRUPT_ENABLE && deny_mask) { regs[off/4] = 1; return; }
    if (off == MAC_INTERRUPT_ENABLE && deny_mac_mask) { regs[off/4] = 1; return; }
    if (off == DMA_CH0_TX_CONTROL && deny_tx_stop && !(value & 1)) return;
    if (off == DMA_CH0_RX_CONTROL && deny_rx_stop && !(value & 1)) return;
    if (off == MAC_CONFIGURATION && deny_mac_stop && !(value & 3)) return;
    if (off == DMA_CH0_TX_CONTROL && refuse_start && (value & 1)) return;
    if (off == DMA_CH0_CONTROL && refuse_stride) value |= 7U << 18;
    if (off == DMA_SYSBUS_MODE && refuse_eame) value &= ~(1U << 11);
    if (off == DMA_CH0_STATUS) { regs[off/4] &= ~value; return; }
    if (off == DMA_MODE && (value & 1)) {
        assert(!irq_depth && ctx()->control_lock.held);
        resets++; reset_reads = 0;
        for (unsigned i = 0x1100/4; i < sizeof(regs)/sizeof(regs[0]); i++) regs[i] = 0;
    }
    if (off == MAC_MII_ADDR && (value & 1)) {
        assert(!irq_depth && ctx()->control_lock.held);
        assert(!(value & 2)); /* C45E must be clear for this Clause-22 PHY */
        unsigned addr = (value >> 21) & 31, reg = (value >> 16) & 31;
        if (((value >> 2) & 3) == 1) phy[reg] = regs[MAC_MII_DATA/4] & ~0x8000U;
        else {
            assert(((value >> 2) & 3) == 3);
            regs[MAC_MII_DATA/4] = addr == 1 && !no_phy ? phy[reg] : 0xffff;
            if (reg == 1 && bad_link) regs[MAC_MII_DATA/4] = 0xffff;
        }
        value &= ~1U;
    }
    regs[off/4] = value;
    if (off == DMA_CH0_TXDESC_TAIL_PTR && !hold_tx) transmit();
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
    for (unsigned i = 0; i < GMAC_MAX_INSTANCES; i++) assert(!g_gmac_insts[i].valid);
    memset(&dev, 0, sizeof(dev)); memset(regs, 0, sizeof(regs));
    memset(dma_view, 0, sizeof(dma_view)); memset(phy, 0, sizeof(phy));
    dev.name = "model"; dev.resource = (resource_t){BASE, BASE+sizeof(regs)-1};
    regs[MAC_VERSION/4] = 0x1052; regs[MAC_HW_FEATURE1/4] = (1U<<14)|(4U<<6)|4U;
    phy[2] = 0x1234; phy[3] = 0x5678; phy[1] = 4;
    physical = UINT64_C(0x120000000); /* valid 40-bit DMA above 4 GiB */
    ticks = reads = writes = resets = releases = unmaps = reset_reads = tx_hw = packets = 0;
    fail_action = fail_map = fail_dma_gate = dead = no_phy = bad_link = 0;
    deny_mask = deny_mac_mask = deny_tx_stop = deny_rx_stop = deny_mac_stop = 0;
    stuck_process = stuck_reset = refuse_start = refuse_stride = refuse_eame = 0;
    cancel_mdio = freeze_time = hold_tx = 0;
}
static int unbind(void) {
    int ret;
    if (dev.stop && !dev.stopped) {
        ret = dev.stop(dev.context); if (ret) return ret; dev.stopped = 1;
    }
    ret = starfive_gmac_driver.remove(&dev); if (ret) return ret;
    if (dev.mapped) { dev.mapped = 0; unmaps++; }
    if (dev.release) { dev.release(dev.context); releases++; }
    dev.context = NULL; dev.stop = NULL; dev.release = NULL; return 0;
}
static void retained(starfive_gmac_priv_t *saved) {
    assert(dev.context == saved && dev.drv_priv == saved && saved->valid && saved->owner == &dev);
    assert(dev.mapped && !releases && !unmaps && !saved->stopped);
    saved->tx_buf[0][0] = 0x5a;
    starfive_gmac_priv_t *other = gmac_alloc_instance(&dev);
    assert(other && other != saved && saved->tx_buf[0][0] == 0x5a); starfive_gmac_release(other);
    unsigned count = reads + writes; uint8_t packet[4096] = {0};
    assert(starfive_gmac_class_send(&dev, packet, 64) == -ESHUTDOWN);
    assert(starfive_gmac_class_recv(&dev, packet, sizeof(packet)) == -ESHUTDOWN);
    starfive_gmac_class_poll(&dev); assert(!starfive_gmac_class_mac(&dev));
    assert(reads + writes == count);
}
static void receive(uint32_t status, int expected, size_t maxlen) {
    unsigned i = ctx()->rx_busy;
    dma_desc_t *d = dma_pointer(list_address(1) + i * 64);
    uint64_t buffer = ((uint64_t)d->des1 << 32) | d->des0;
    assert(buffer == va_to_pa(ctx()->rx_buf[i])); memset(dma_pointer(buffer), 0xa6, 1536);
    /* RX writeback replaces all four read-format words. CPU still sees OWN. */
    assert(ctx()->rx_desc[i].des3 & (1U<<31));
    d->des0 = 0xbad0; d->des1 = 0xbad1; d->des2 = 0xbad2; d->des3 = status;
    uint8_t output[4096]; memset(output, 0x55, sizeof(output));
    assert(starfive_gmac_recv(BASE, output, maxlen) == expected);
    assert(output[expected > 0 ? (unsigned)expected : 0] == 0x55);
    for (int j = 0; j < expected; j++) assert(output[j] == 0xa6);
    assert(d->des3 == 0x81000000U && !d->des2);
    assert((((uint64_t)d->des1 << 32) | d->des0) == buffer);
    assert(regs[DMA_CH0_RXDESC_TAIL_PTR/4] == (uint32_t)va_to_pa(&ctx()->rx_desc[i]));
}
int main(void) {
    reset(); dev.resource.end = BASE+3;
    assert(starfive_gmac_driver.probe(&dev) == -ENODEV && !dev.context && !reads && !writes);
    reset(); fail_action = 1;
    assert(starfive_gmac_driver.probe(&dev) == -ENOMEM && !dev.drv_priv && !dev.mapped); assert(!unbind());
    reset(); fail_map = 1;
    assert(starfive_gmac_driver.probe(&dev) == -ENODEV); assert(!unbind() && releases == 1 && !reads && !writes);
    reset(); fail_dma_gate = 1;
    assert(starfive_gmac_driver.probe(&dev) == -EOPNOTSUPP); assert(!unbind() && !writes);
    reset(); physical = UINT64_C(0xffffffc0) - offsetof(starfive_gmac_priv_t, tx_desc);
    assert(starfive_gmac_driver.probe(&dev) == -ERANGE); assert(!unbind() && !writes);
    reset(); regs[MAC_HW_FEATURE1/4] &= ~(3U<<14); /* 32-bit device, high storage */
    assert(starfive_gmac_driver.probe(&dev) == -ERANGE); assert(!unbind() && !writes);
    reset(); regs[MAC_VERSION/4] = 0x37;
    assert(starfive_gmac_driver.probe(&dev) == -ENODEV); assert(!unbind() && !writes);
    reset(); dead = 1;
    assert(starfive_gmac_driver.probe(&dev) == -ENODEV); assert(!unbind() && !writes);
    reset(); stuck_reset = 1;
    assert(starfive_gmac_driver.probe(&dev) == -ETIMEDOUT);
    starfive_gmac_priv_t *saved = ctx(); assert(unbind() == -ETIMEDOUT); retained(saved);
    stuck_reset = 0; assert(!unbind() && releases == 1 && unmaps == 1);
    reset(); no_phy = 1;
    assert(starfive_gmac_driver.probe(&dev) == -ENODEV); assert(!unbind());
    reset(); bad_link = 1;
    assert(starfive_gmac_driver.probe(&dev) < 0 && !ctx()->ready); assert(!unbind());
    reset(); refuse_start = 1;
    assert(starfive_gmac_driver.probe(&dev) == -EIO); assert(!unbind());
    reset(); refuse_stride = 1; /* seven contains all set bits of six */
    assert(starfive_gmac_driver.probe(&dev) == -EIO); assert(!unbind());
    reset(); refuse_eame = 1;
    assert(starfive_gmac_driver.probe(&dev) == -EIO); assert(!unbind());
    reset(); cancel_mdio = 1;
    assert(starfive_gmac_driver.probe(&dev) == -ESHUTDOWN); saved = ctx(); retained(saved); assert(!unbind());

    reset(); assert(!starfive_gmac_driver.probe(&dev));
    assert(dev.dma_mask == UINT64_C(0xffffffffff));
    assert(regs[0x1110/4] == 1 && regs[0x1118/4] == 1);
    assert((regs[MTL_TXQ0_OPERATION_MODE/4] >> 16) == 7);
    assert((regs[MTL_RXQ0_OPERATION_MODE/4] >> 20) == 7 && regs[MAC_RXQ_CTRL0/4] == 2);
    assert(starfive_gmac_driver.remove(&dev) == -EBUSY);
    uint8_t packet[80]; memset(packet, 0x42, sizeof(packet));
    assert(starfive_gmac_send(BASE, NULL, 80) == -EINVAL);
    assert(starfive_gmac_send(BASE, packet, 0) == -EINVAL);
    for (unsigned i = 0; i < 35; i++) assert(!starfive_gmac_send(BASE, packet, i ? 80 : 1));
    assert(packets == 35 && tx_hw == 3);
    hold_tx = 1;
    for (unsigned i = 0; i < 15; i++) assert(!starfive_gmac_send(BASE, packet, 80));
    unsigned count = writes;
    assert(starfive_gmac_send(BASE, packet, 80) == -EAGAIN && writes == count);
    hold_tx = 0; transmit(); assert(!starfive_gmac_send(BASE, packet, 80));
    for (unsigned i = 0; i < 35; i++) receive(0x30000000U|84U, 80, 4096);
    receive(0x30000000U|84U, 7, 7);
    receive(0x30000000U|1536U, 1532, 4096);
    receive(0x30000000U|1537U, -EIO, 4096);
    receive(0x30000000U|0x7fffU, -EIO, 4096);
    receive(0x30000000U|3U, -EIO, 4096);
    receive(0x10000000U|84U, -EIO, 4096);
    receive(0x30008000U|84U, -EIO, 4096);
    receive(0x70000000U|84U, -EIO, 4096);
    assert(!unbind() && releases == 1 && unmaps == 1);

    reset(); assert(!starfive_gmac_driver.probe(&dev)); saved = ctx();
    mutex_lock(&saved->control_lock); assert(unbind() == -EBUSY); retained(saved);
    mutex_unlock(&saved->control_lock);
    uint64_t flags = spin_lock_irqsave(&saved->lock); assert(unbind() == -EBUSY);
    spin_unlock_irqrestore(&saved->lock, flags); retained(saved);
    deny_mask = 1; assert(unbind() == -ETIMEDOUT); retained(saved); deny_mask = 0;
    deny_mac_mask = 1; assert(unbind() == -ETIMEDOUT); retained(saved); deny_mac_mask = 0;
    deny_tx_stop = 1; assert(unbind() == -ETIMEDOUT); retained(saved); deny_tx_stop = 0;
    deny_mac_stop = 1; assert(unbind() == -ETIMEDOUT); retained(saved); deny_mac_stop = 0;
    deny_rx_stop = 1; assert(unbind() == -ETIMEDOUT); retained(saved); deny_rx_stop = 0;
    stuck_process = 1; assert(unbind() == -ETIMEDOUT); retained(saved); stuck_process = 0;
    dead = 1; assert(unbind() == -ENODEV); retained(saved); dead = 0;
    stuck_reset = 1; assert(unbind() == -ETIMEDOUT); retained(saved); stuck_reset = 0;
    assert(!unbind() && releases == 1 && unmaps == 1);
    reset(); assert(!starfive_gmac_driver.probe(&dev)); saved = ctx();
    freeze_time = stuck_reset = 1; assert(unbind() == -ETIMEDOUT); retained(saved);
    freeze_time = stuck_reset = 0; assert(!unbind());
    /* A 32-bit implementation remains usable when every address is low. */
    reset(); physical = UINT64_C(0x20000000); regs[MAC_HW_FEATURE1/4] &= ~(3U<<14);
    assert(!starfive_gmac_driver.probe(&dev)); assert(!starfive_gmac_send(BASE, packet, 80)); assert(!unbind());
    puts("STARFIVE_GMAC_LIFETIME_OK: high DMA, cache-isolated rings, RX bounds, partial probe, retained stop and retry");
}
