#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "net/lwip_stack.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/string.h"
#include "mm/mm.h"

#define E1000_VENDOR_INTEL 0x8086U
/* Common e1000 / e1000e PCI device IDs (Linux e1000/e1000e supported set). */
#define E1000_DEVICE_82540EM 0x100EU   /* 82540EM (QEMU default) */
#define E1000_DEVICE_82545EM 0x100FU   /* 82545EM / 82545GM */
#define E1000_DEVICE_82546EB 0x1010U   /* 82546EB */
#define E1000_DEVICE_82541PI 0x107CU   /* 82541PI / 82547GI */
#define E1000_DEVICE_82574L  0x10D3U   /* 82574L (e1000e) */

#define E1000_CTRL   0x0000U
#define E1000_STATUS 0x0008U
#define E1000_ICR    0x00C0U
#define E1000_IMS    0x00D0U
#define E1000_IMC    0x00D8U
#define E1000_RCTL   0x0100U
#define E1000_TCTL   0x0400U
#define E1000_TIPG   0x0410U
#define E1000_RDBAL  0x2800U
#define E1000_RDBAH  0x2804U
#define E1000_RDLEN  0x2808U
#define E1000_RDH    0x2810U
#define E1000_RDT    0x2818U
#define E1000_TDBAL  0x3800U
#define E1000_TDBAH  0x3804U
#define E1000_TDLEN  0x3808U
#define E1000_TDH    0x3810U
#define E1000_TDT    0x3818U
#define E1000_RAL0   0x5400U
#define E1000_RAH0   0x5404U

#define E1000_CTRL_SLU       (1U << 6)
#define E1000_RCTL_EN        (1U << 1)
#define E1000_RCTL_BAM       (1U << 15)
#define E1000_RCTL_SECRC     (1U << 26)
#define E1000_TCTL_EN        (1U << 1)
#define E1000_TCTL_PSP       (1U << 3)
#define E1000_RXD_STAT_DD    (1U << 0)
#define E1000_RXD_STAT_EOP   (1U << 1)
#define E1000_TXD_CMD_EOP    (1U << 0)
#define E1000_TXD_CMD_IFCS   (1U << 1)
#define E1000_TXD_CMD_RS     (1U << 3)
#define E1000_TXD_STAT_DD    (1U << 0)

/* Interrupt causes enabled in IMS when a line is registered: TX descriptor
 * write-back, link status change, RX overrun, and the RX timer.  ICR is
 * read-to-clear, which is also the top-half acknowledge. */
#define E1000_IMS_TXDW       (1U << 0)
#define E1000_IMS_LSC        (1U << 2)
#define E1000_IMS_RXO        (1U << 6)
#define E1000_IMS_RXT0       (1U << 7)
#define E1000_IMS_USED       (E1000_IMS_TXDW | E1000_IMS_LSC | \
                              E1000_IMS_RXO | E1000_IMS_RXT0)

#define E1000_RING_SIZE 64U
#define E1000_BUF_SIZE  2048U

typedef struct {
    uint64_t address;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    uint64_t address;
    uint16_t length;
    uint8_t cso;
    uint8_t command;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

typedef struct {
    uintptr_t regs;
    uint8_t mac[6];
    uint32_t rx_next;
    uint32_t tx_next;
    spinlock_t lock;
    int irq;
    int irq_registered;
    e1000_rx_desc_t rx[E1000_RING_SIZE] ALIGNED(16);
    e1000_tx_desc_t tx[E1000_RING_SIZE] ALIGNED(16);
    uint8_t rx_buf[E1000_RING_SIZE][E1000_BUF_SIZE] ALIGNED(16);
    uint8_t tx_buf[E1000_RING_SIZE][E1000_BUF_SIZE] ALIGNED(16);
} e1000_device_t;

static e1000_device_t g_e1000;

static inline uint32_t e1000_read(e1000_device_t *nic, uint32_t reg)
{
    return readl((const volatile void *)(nic->regs + reg));
}

static inline void e1000_write(e1000_device_t *nic, uint32_t reg, uint32_t value)
{
    writel(value, (volatile void *)(nic->regs + reg));
}

/* E1000_IRQ_MODEL:
 * - The top-half acknowledges by reading ICR (read-to-clear), resolves the
 *   device's netif index, and runs the same bounded RX drain the virtio-net
 *   IRQ path uses, under g_lwip_lock.  A shared line with no pending cause
 *   costs one register read.
 * - IMS is unmasked only after the handler is registered; without a handler
 *   the device keeps its causes masked and the class .poll hook remains the
 *   only progress path. */
static int e1000_irq_handler(int irq, void *priv) {
    (void)irq;
    device_t *dev = (device_t *)priv;
    e1000_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic)
        return 0;
    uint32_t icr = e1000_read(nic, E1000_ICR);
    if (!icr)
        return 0;
    int net_idx = -1;
    for (int i = 0; i < 8; i++) {
        device_t *cur = device_find_by_class(DEV_CLASS_NET, i);
        if (!cur)
            break;
        if (cur == dev) {
            net_idx = i;
            break;
        }
    }
    uint64_t flags = a20_lwip_lock();
    if (net_idx >= 0)
        a20_lwip_process_netif_irq_locked(net_idx);
    else
        a20_lwip_signal_rx_pending();
    a20_lwip_unlock(flags);
    return 0;
}

static void e1000_poll(device_t *dev)
{
    e1000_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic)
        return;
    (void)e1000_read(nic, E1000_ICR);
    arch_dma_sync_for_cpu(nic->tx, sizeof(nic->tx));
}

static int e1000_send(device_t *dev, const void *packet, size_t length)
{
    e1000_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic || !packet || length == 0 || length > E1000_BUF_SIZE)
        return -1;

    uint64_t flags = spin_lock_irqsave(&nic->lock);
    uint32_t slot = nic->tx_next;
    arch_dma_sync_for_cpu(&nic->tx[slot], sizeof(nic->tx[slot]));
    if (!(nic->tx[slot].status & E1000_TXD_STAT_DD)) {
        spin_unlock_irqrestore(&nic->lock, flags);
        return -1;
    }

    memcpy(nic->tx_buf[slot], packet, length);
    arch_dma_sync_for_device(nic->tx_buf[slot], length);
    nic->tx[slot].length = (uint16_t)length;
    nic->tx[slot].cso = 0;
    nic->tx[slot].command = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS |
                            E1000_TXD_CMD_RS;
    nic->tx[slot].status = 0;
    nic->tx[slot].css = 0;
    nic->tx[slot].special = 0;
    arch_dma_sync_for_device(&nic->tx[slot], sizeof(nic->tx[slot]));
    wmb();

    nic->tx_next = (slot + 1U) % E1000_RING_SIZE;
    e1000_write(nic, E1000_TDT, nic->tx_next);
    spin_unlock_irqrestore(&nic->lock, flags);
    return (int)length;
}

static int e1000_recv(device_t *dev, void *buffer, size_t max_length)
{
    e1000_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic || !buffer || max_length == 0)
        return -1;

    uint64_t flags = spin_lock_irqsave(&nic->lock);
    uint32_t slot = nic->rx_next;
    arch_dma_sync_for_cpu(&nic->rx[slot], sizeof(nic->rx[slot]));
    if (!(nic->rx[slot].status & E1000_RXD_STAT_DD)) {
        spin_unlock_irqrestore(&nic->lock, flags);
        return 0;
    }

    size_t length = nic->rx[slot].length;
    if (length > max_length)
        length = max_length;
    if (nic->rx[slot].errors || !(nic->rx[slot].status & E1000_RXD_STAT_EOP))
        length = 0;
    if (length) {
        arch_dma_sync_for_cpu(nic->rx_buf[slot], length);
        memcpy(buffer, nic->rx_buf[slot], length);
    }

    nic->rx[slot].status = 0;
    nic->rx[slot].errors = 0;
    arch_dma_sync_for_device(&nic->rx[slot], sizeof(nic->rx[slot]));
    e1000_write(nic, E1000_RDT, slot);
    nic->rx_next = (slot + 1U) % E1000_RING_SIZE;
    spin_unlock_irqrestore(&nic->lock, flags);
    return (int)length;
}

static const uint8_t *e1000_mac(device_t *dev)
{
    e1000_device_t *nic = dev ? dev->drv_priv : NULL;
    return nic ? nic->mac : NULL;
}

static int e1000_probe(device_t *dev)
{
    if (pci_enable_and_assign_bars(dev) < 0)
        return -1;
    resource_t *bar = pci_get_bar_resource(dev, 0);
    if (!bar || bar->end < bar->start || bar->end - bar->start + 1U < 0x6000U)
        return -1;

    e1000_device_t *nic = &g_e1000;
    memset(nic, 0, sizeof(*nic));
    spin_init(&nic->lock);
    nic->regs = (uintptr_t)bar->start;

    uint32_t ral = e1000_read(nic, E1000_RAL0);
    uint32_t rah = e1000_read(nic, E1000_RAH0);
    nic->mac[0] = (uint8_t)ral;
    nic->mac[1] = (uint8_t)(ral >> 8);
    nic->mac[2] = (uint8_t)(ral >> 16);
    nic->mac[3] = (uint8_t)(ral >> 24);
    nic->mac[4] = (uint8_t)rah;
    nic->mac[5] = (uint8_t)(rah >> 8);
    if (!(rah & (1U << 31)))
        return -1;

    e1000_write(nic, E1000_IMC, 0xFFFFFFFFU);
    (void)e1000_read(nic, E1000_ICR);
    e1000_write(nic, E1000_CTRL, e1000_read(nic, E1000_CTRL) | E1000_CTRL_SLU);

    for (uint32_t i = 0; i < E1000_RING_SIZE; i++) {
        nic->rx[i].address = va_to_pa(nic->rx_buf[i]);
        nic->tx[i].address = va_to_pa(nic->tx_buf[i]);
        nic->tx[i].status = E1000_TXD_STAT_DD;
    }
    arch_dma_sync_for_device(nic->rx_buf, sizeof(nic->rx_buf));
    arch_dma_sync_for_device(nic->tx_buf, sizeof(nic->tx_buf));
    arch_dma_sync_for_device(nic->rx, sizeof(nic->rx));
    arch_dma_sync_for_device(nic->tx, sizeof(nic->tx));

    uint64_t rx_pa = va_to_pa(nic->rx);
    e1000_write(nic, E1000_RDBAL, (uint32_t)rx_pa);
    e1000_write(nic, E1000_RDBAH, (uint32_t)(rx_pa >> 32));
    e1000_write(nic, E1000_RDLEN, sizeof(nic->rx));
    e1000_write(nic, E1000_RDH, 0);
    e1000_write(nic, E1000_RDT, E1000_RING_SIZE - 1U);

    uint64_t tx_pa = va_to_pa(nic->tx);
    e1000_write(nic, E1000_TDBAL, (uint32_t)tx_pa);
    e1000_write(nic, E1000_TDBAH, (uint32_t)(tx_pa >> 32));
    e1000_write(nic, E1000_TDLEN, sizeof(nic->tx));
    e1000_write(nic, E1000_TDH, 0);
    e1000_write(nic, E1000_TDT, 0);

    e1000_write(nic, E1000_TIPG, 10U | (8U << 10) | (6U << 20));
    e1000_write(nic, E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                (0x10U << 4) | (0x40U << 12));
    e1000_write(nic, E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM |
                E1000_RCTL_SECRC);

    dev->drv_priv = nic;
    int irq = pci_intx_irq(dev);
    if (irq >= 0) {
        if (request_irq((uint32_t)irq, e1000_irq_handler, IRQF_SHARED,
                        dev) == 0) {
            nic->irq = irq;
            nic->irq_registered = 1;
            /* Unmask device causes only with the handler in place. */
            e1000_write(nic, E1000_IMS, E1000_IMS_USED);
        } else {
            kinfo("[E1000] IRQ %d registration failed; using polling\n",
                  irq);
        }
    }
    kinfo("[E1000] ready: mac=%02x:%02x:%02x:%02x:%02x:%02x link=%s irq=%d\n",
          nic->mac[0], nic->mac[1], nic->mac[2], nic->mac[3], nic->mac[4],
          nic->mac[5], (e1000_read(nic, E1000_STATUS) & 2U) ? "up" : "down",
          nic->irq_registered ? nic->irq : -1);
    return 0;
}

static int e1000_remove(device_t *dev)
{
    e1000_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic)
        return 0;
    /* Mask device causes before releasing the handler. */
    e1000_write(nic, E1000_IMC, 0xFFFFFFFFU);
    if (nic->irq_registered)
        free_irq((uint32_t)nic->irq, dev);
    e1000_write(nic, E1000_RCTL, 0);
    e1000_write(nic, E1000_TCTL, 0);
    dev->drv_priv = NULL;
    memset(nic, 0, sizeof(*nic));
    return 0;
}

static const net_dev_ops_t e1000_ops = {
    .send = e1000_send,
    .recv = e1000_recv,
    .mac = e1000_mac,
    .poll = e1000_poll,
};

static const device_id_t e1000_ids[] = {
    { .vendor = E1000_VENDOR_INTEL, .device = E1000_DEVICE_82540EM,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = E1000_VENDOR_INTEL, .device = E1000_DEVICE_82545EM,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = E1000_VENDOR_INTEL, .device = E1000_DEVICE_82546EB,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = E1000_VENDOR_INTEL, .device = E1000_DEVICE_82541PI,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = E1000_VENDOR_INTEL, .device = E1000_DEVICE_82574L,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static driver_t e1000_driver = {
    .name = "e1000",
    .id_table = e1000_ids,
    .bus = &pci_bus,
    .probe = e1000_probe,
    .remove = e1000_remove,
    .class_ops = &e1000_ops,
    .class_type = DEV_CLASS_NET,
};

DRIVER_REGISTER(e1000_driver);
