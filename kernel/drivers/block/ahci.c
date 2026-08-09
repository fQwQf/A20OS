#ifdef CONFIG_AHCI

#include "drivers/block/ahci.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/errno.h"
#include "core/sync.h"
#include "core/timer.h"
#include "proc/proc.h"

#define AHCI_MAX_PORTS          32U
#define AHCI_CMD_SLOTS          32U
#define AHCI_SECTOR_SIZE        512U
#define AHCI_TRANSFER_SECTORS   128U
#define AHCI_TRANSFER_BYTES     (AHCI_TRANSFER_SECTORS * AHCI_SECTOR_SIZE)
#define AHCI_TIMEOUT_MS         5000U

#define AHCI_GHC                0x04U
#define AHCI_IS                 0x08U
#define AHCI_PI                 0x0CU
#define AHCI_PORT_BASE          0x100U
#define AHCI_PORT_STRIDE        0x80U

#define AHCI_PXCLB              0x00U
#define AHCI_PXCLBU             0x04U
#define AHCI_PXFB               0x08U
#define AHCI_PXFBU              0x0CU
#define AHCI_PXIS               0x10U
#define AHCI_PXIE               0x14U
#define AHCI_PXCMD              0x18U
#define AHCI_PXTFD              0x20U
#define AHCI_PXSSTS             0x28U
#define AHCI_PXSCTL             0x2CU
#define AHCI_PXCI               0x38U

#define AHCI_GHC_HR             (1U << 0)
#define AHCI_GHC_IE             (1U << 1)
#define AHCI_GHC_AE             (1U << 31)
#define AHCI_PXCMD_ST           (1U << 0)
#define AHCI_PXCMD_FRE          (1U << 4)
#define AHCI_PXCMD_FR           (1U << 14)
#define AHCI_PXCMD_CR           (1U << 15)
#define AHCI_PXTFD_BSY          (1U << 7)
#define AHCI_PXTFD_DRQ          (1U << 3)
#define AHCI_PXIS_TFES          (1U << 30)

#define ATA_CMD_IDENTIFY        0xECU
#define ATA_CMD_READ_DMA_EXT    0x25U
#define ATA_CMD_WRITE_DMA_EXT   0x35U

/* Hybrid completion window: TCG completions usually land within a
 * millisecond; beyond it the submitter parks on the port wait queue
 * instead of busy-polling PxCI for the whole command. */
#define AHCI_HYBRID_PRE_POLL_US 800U
/* Bounded park chunk: a hypothetical missed wake degrades to a re-check. */
#define AHCI_PARK_CHUNK_MS      50U

typedef struct __attribute__((packed)) ahci_cmd_header {
    uint16_t flags;
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} ahci_cmd_header_t;

typedef struct __attribute__((packed)) ahci_prdt {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc;
} ahci_prdt_t;

typedef struct __attribute__((packed)) ahci_cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    ahci_prdt_t prdt[8];
} ahci_cmd_table_t;

typedef struct __attribute__((aligned(1024))) ahci_port {
    uintptr_t regs;
    uint32_t port_no;
    uint64_t cmd_list_dma;
    uint64_t rfis_dma;
    uint64_t tables_dma;
    uint64_t transfer_dma;
    ahci_cmd_header_t *cmd_list;
    void *rfis;
    ahci_cmd_table_t *tables;
    uint8_t *transfer;
    uint64_t capacity;
    /* AHCI_IRQ_MODEL:
     * - The IRQ top-half write-clears PxIS and records the bits it
     *   consumed in last_is, because the waiter must still observe TFES
     *   after the hardware status register has been acknowledged.
     * - Parked submitters sleep on waiters; the handler collects them
     *   after recording last_is.  Only slot 0 is ever in flight (the
     *   port mutex serializes commands), so one status word suffices. */
    volatile uint32_t last_is;
    volatile int irq_seen;
    int irq;
    int irq_registered;
    wait_queue_t waiters;
    /* Sleepable mutex: the IRQ handler never takes it, and the command
     * wait path parks while holding it. */
    mutex_t lock;
    block_dev_t block;
} ahci_port_t;

static ahci_port_t g_ahci_port;
static int g_ahci_ready;

static volatile void *ahci_reg(uintptr_t base, uint32_t off) {
    return (volatile void *)(base + off);
}

static uint32_t ahci_read(ahci_port_t *port, uint32_t off) {
    return readl(ahci_reg(port->regs, off));
}

static void ahci_write(ahci_port_t *port, uint32_t off, uint32_t value) {
    writel(value, ahci_reg(port->regs, off));
}

static int ahci_wait_clear(ahci_port_t *port, uint32_t off, uint32_t mask) {
    for (unsigned ms = 0; ms < AHCI_TIMEOUT_MS; ms++) {
        if ((ahci_read(port, off) & mask) == 0)
            return 0;
        mdelay(1);
    }
    return -1;
}

/* Probe-time port readiness: a real device clears BSY/DRQ within a few ms
 * once PHY link is up, so a short bound is enough to tell it from a phantom
 * port (QEMU reports DET=3 on unused ports with BSY never clearing).  The
 * full AHCI_TIMEOUT_MS is still applied by ahci_submit() on real commands. */
#define AHCI_PROBE_READY_MS  100U

static int ahci_wait_ready(ahci_port_t *port) {
    for (unsigned ms = 0; ms < AHCI_TIMEOUT_MS; ms++) {
        if ((ahci_read(port, AHCI_PXTFD) & (AHCI_PXTFD_BSY | AHCI_PXTFD_DRQ)) == 0)
            return 0;
        mdelay(1);
    }
    return -1;
}

static int ahci_wait_ready_short(ahci_port_t *port) {
    for (unsigned ms = 0; ms < AHCI_PROBE_READY_MS; ms++) {
        if ((ahci_read(port, AHCI_PXTFD) & (AHCI_PXTFD_BSY | AHCI_PXTFD_DRQ)) == 0)
            return 0;
        mdelay(1);
    }
    return -1;
}

static int ahci_stop_port(ahci_port_t *port) {
    uint32_t cmd = ahci_read(port, AHCI_PXCMD);
    ahci_write(port, AHCI_PXCMD, cmd & ~(AHCI_PXCMD_ST | AHCI_PXCMD_FRE));
    return ahci_wait_clear(port, AHCI_PXCMD, AHCI_PXCMD_CR | AHCI_PXCMD_FR);
}

static int ahci_start_port(ahci_port_t *port) {
    if (ahci_wait_clear(port, AHCI_PXCMD, AHCI_PXCMD_CR) != 0)
        return -1;
    uint32_t cmd = ahci_read(port, AHCI_PXCMD);
    ahci_write(port, AHCI_PXCMD, cmd | AHCI_PXCMD_FRE | AHCI_PXCMD_ST);
    return 0;
}

static int ahci_submit(ahci_port_t *port, uint8_t command, uint64_t lba,
                       uint16_t sectors, int write, uint64_t dma, size_t bytes) {
    if (!sectors || bytes > AHCI_TRANSFER_BYTES)
        return -1;
    if (ahci_wait_ready(port) != 0)
        return -1;

    ahci_cmd_header_t *header = &port->cmd_list[0];
    ahci_cmd_table_t *table = &port->tables[0];
    memset(header, 0, sizeof(*header));
    memset(table, 0, sizeof(*table));

    header->flags = (uint16_t)(5U | (write ? (1U << 6) : 0));
    header->prdtl = 1;
    header->ctba = (uint32_t)port->tables_dma;
    header->ctbau = (uint32_t)(port->tables_dma >> 32);

    table->cfis[0] = 0x27U;
    table->cfis[1] = 0x80U;
    table->cfis[2] = command;
    table->cfis[4] = (uint8_t)lba;
    table->cfis[5] = (uint8_t)(lba >> 8);
    table->cfis[6] = (uint8_t)(lba >> 16);
    table->cfis[7] = 0x40U;
    table->cfis[8] = (uint8_t)(lba >> 24);
    table->cfis[9] = (uint8_t)(lba >> 32);
    table->cfis[10] = (uint8_t)(lba >> 40);
    table->cfis[12] = (uint8_t)sectors;
    table->cfis[13] = (uint8_t)(sectors >> 8);

    table->prdt[0].dba = (uint32_t)dma;
    table->prdt[0].dbau = (uint32_t)(dma >> 32);
    table->prdt[0].dbc = (uint32_t)(bytes - 1U) | (1U << 31);

    dma_sync_for_device(port->cmd_list, 1024U);
    dma_sync_for_device(table, sizeof(*table));
    dma_sync_for_device(port->transfer, bytes);
    port->last_is = 0;
    port->irq_seen = 0;
    ahci_write(port, AHCI_PXIS, 0xFFFFFFFFU);
    ahci_write(port, AHCI_PXCI, 1U);

    uint64_t start = timer_get_ticks();
    uint64_t deadline = start + MS_TO_TICKS(AHCI_TIMEOUT_MS);
    uint64_t pre_poll_until = start + US_TO_TICKS(AHCI_HYBRID_PRE_POLL_US);

    for (;;) {
        uint32_t is = ahci_read(port, AHCI_PXIS) | port->last_is;
        if (is & AHCI_PXIS_TFES) {
            ahci_write(port, AHCI_PXIS, 0xFFFFFFFFU);
            port->last_is = 0;
            return -1;
        }
        if ((ahci_read(port, AHCI_PXCI) & 1U) == 0) {
            port->last_is = 0;
            dma_sync_for_cpu(port->transfer, bytes);
            return 0;
        }
        if (!port->irq_registered) {
            if (timer_get_ticks() >= deadline)
                return -1;
            udelay(1000);
            continue;
        }
        uint64_t now = timer_get_ticks();
        if (now >= deadline)
            return -1;
        if (now < pre_poll_until) {
            udelay(20);
            continue;
        }
        /* Park until the completion IRQ; the bounded chunk turns a
         * hypothetical missed wake into a re-check instead of a stall. */
        uint64_t chunk = now + MS_TO_TICKS(AHCI_PARK_CHUNK_MS);
        if (chunk > deadline)
            chunk = deadline;
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, chunk);
        wait_queue_entry_t entry = {0};
        wait_queue_link(&port->waiters, &entry, token, 0);
        /* Re-check after linking so a completion that raced the link does
         * not sleep. */
        if ((ahci_read(port, AHCI_PXCI) & 1U) == 0 ||
            (ahci_read(port, AHCI_PXIS) & AHCI_PXIS_TFES) ||
            (port->last_is & AHCI_PXIS_TFES)) {
            wait_queue_unlink(&port->waiters, &entry);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            continue;
        }
        (void)proc_park_commit(token);
        wait_queue_unlink(&port->waiters, &entry);
        proc_park_finish(token);
    }
}

static int ahci_rw(ahci_port_t *port, uint64_t lba, void *buf, size_t count,
                   int write) {
    if (!port || !buf || !count || lba >= port->capacity || count > port->capacity - lba)
        return -1;
    mutex_lock(&port->lock);
    while (count) {
        size_t sectors = count > AHCI_TRANSFER_SECTORS ? AHCI_TRANSFER_SECTORS : count;
        size_t bytes = sectors * AHCI_SECTOR_SIZE;
        if (write)
            memcpy(port->transfer, buf, bytes);
        if (ahci_submit(port, write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT,
                        lba, (uint16_t)sectors, write, port->transfer_dma, bytes) != 0) {
            mutex_unlock(&port->lock);
            return -1;
        }
        if (!write)
            memcpy(buf, port->transfer, bytes);
        lba += sectors;
        count -= sectors;
        buf = (uint8_t *)buf + bytes;
    }
    mutex_unlock(&port->lock);
    return 0;
}

static int ahci_block_read(block_dev_t *dev, uint64_t lba, void *buf, size_t count) {
    return ahci_rw((ahci_port_t *)dev->priv, lba, buf, count, 0);
}

static int ahci_block_write(block_dev_t *dev, uint64_t lba, const void *buf, size_t count) {
    return ahci_rw((ahci_port_t *)dev->priv, lba, (void *)buf, count, 1);
}

block_dev_t *ahci_get_dev(int idx) {
    if (idx != 0 || !g_ahci_ready)
        return NULL;
    return &g_ahci_port.block;
}

static int ahci_irq_handler(int irq, void *priv) {
    (void)irq;
    ahci_port_t *port = (ahci_port_t *)priv;
    if (!port)
        return 0;
    /* Top-half only: acknowledge and record the status bits, then wake the
     * parked submitter, which re-checks PxCI/last_is itself.  Recording
     * last_is before the write-clear keeps TFES observable to the waiter. */
    uint32_t is = ahci_read(port, AHCI_PXIS);
    if (is) {
        port->last_is |= is;
        ahci_write(port, AHCI_PXIS, is);
        port->irq_seen = 1;
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        (void)wait_queue_collect_all(&port->waiters, 0, PROC_WAKE_EVENT,
                                     &wake_q, NULL);
        (void)proc_wake_q_flush(&wake_q);
    }
    return 0;
}

static int ahci_port_present(ahci_port_t *port) {
    uint32_t ssts = ahci_read(port, AHCI_PXSSTS);
    /* DET==3 means PHY link is up, but QEMU reports that on unused ports too
     * with BSY stuck set.  Bound the readiness wait to probe time so phantom
     * ports do not stall boot for the full AHCI_TIMEOUT_MS. */
    return (ssts & 0x0FU) == 3U && ahci_wait_ready_short(port) == 0;
}

static int ahci_identify(ahci_port_t *port) {
    if (ahci_submit(port, ATA_CMD_IDENTIFY, 0, 1, 0, port->transfer_dma,
                    AHCI_SECTOR_SIZE) != 0)
        return -1;

    uint16_t *id = (uint16_t *)port->transfer;
    uint64_t lba48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                     ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    uint64_t lba28 = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    port->capacity = lba48 ? lba48 : lba28;
    return port->capacity ? 0 : -1;
}

static int ahci_probe(device_t *dev) {
    int ret = 0;
    if (g_ahci_ready || pci_enable_and_assign_bars(dev) < 0)
        return -ENODEV;

    resource_t *abar = device_get_resource(dev, RES_MMIO, 0);
    if (!abar)
        return -ENODEV;

    ahci_port_t *port = &g_ahci_port;
    memset(port, 0, sizeof(*port));
    port->regs = (uintptr_t)abar->start;
    port->irq = -1;
    mutex_init(&port->lock);
    wait_queue_init(&port->waiters);

    uint32_t ghc = readl(ahci_reg(port->regs, AHCI_GHC));
    writel(ghc | AHCI_GHC_AE | AHCI_GHC_HR, ahci_reg(port->regs, AHCI_GHC));
    for (unsigned ms = 0; ms < AHCI_TIMEOUT_MS; ms++) {
        if ((readl(ahci_reg(port->regs, AHCI_GHC)) & AHCI_GHC_HR) == 0)
            break;
        mdelay(1);
        if (ms + 1U == AHCI_TIMEOUT_MS)
            return -ETIMEDOUT;
    }
    writel(AHCI_GHC_AE, ahci_reg(port->regs, AHCI_GHC));

    uint32_t pi = readl(ahci_reg(port->regs, AHCI_PI));
    unsigned ports = 0;
    for (uint32_t n = 0; n < AHCI_MAX_PORTS; n++)
        if (pi & (1U << n))
            ports++;
    printf("[AHCI] controller found, ports=%u\n", ports);

    for (uint32_t n = 0; n < AHCI_MAX_PORTS; n++) {
        if (!(pi & (1U << n)))
            continue;
        port->port_no = n;
        port->regs = (uintptr_t)abar->start + AHCI_PORT_BASE + n * AHCI_PORT_STRIDE;
        ahci_write(port, AHCI_PXSCTL, 0x301U);
        mdelay(1);
        ahci_write(port, AHCI_PXSCTL, 0);
        mdelay(10);
        if (ahci_port_present(port))
            break;
        port->port_no = AHCI_MAX_PORTS;
    }
    if (port->port_no == AHCI_MAX_PORTS)
        return -ENODEV;

    port->cmd_list = dma_alloc_coherent(1024U, &port->cmd_list_dma);
    port->rfis = dma_alloc_coherent(256U, &port->rfis_dma);
    port->tables = dma_alloc_coherent(AHCI_CMD_SLOTS * sizeof(*port->tables), &port->tables_dma);
    port->transfer = dma_alloc_coherent(AHCI_TRANSFER_BYTES, &port->transfer_dma);
    if (!port->cmd_list || !port->rfis || !port->tables || !port->transfer) {
        ret = -ENOMEM;
        goto fail;
    }

    if (ahci_stop_port(port) != 0) {
        ret = -ETIMEDOUT;
        goto fail;
    }
    ahci_write(port, AHCI_PXCLB, (uint32_t)port->cmd_list_dma);
    ahci_write(port, AHCI_PXCLBU, (uint32_t)(port->cmd_list_dma >> 32));
    ahci_write(port, AHCI_PXFB, (uint32_t)port->rfis_dma);
    ahci_write(port, AHCI_PXFBU, (uint32_t)(port->rfis_dma >> 32));
    ahci_write(port, AHCI_PXIS, 0xFFFFFFFFU);
    /* Port interrupts stay disabled until a handler is registered: the
     * polling completion path reads PxCI/PxIS directly, and a device that
     * asserts a shared level line without a handler would storm it. */
    ahci_write(port, AHCI_PXIE, 0);
    if (ahci_start_port(port) != 0 || ahci_identify(port) != 0) {
        ret = -EIO;
        goto fail;
    }

    int irq = pci_intx_irq(dev);
    if (irq >= 0) {
        if (request_irq((uint32_t)irq, ahci_irq_handler, IRQF_SHARED,
                        port) == 0) {
            port->irq = irq;
            port->irq_registered = 1;
            /* Unmask device interrupts only with the handler in place. */
            ahci_write(port, AHCI_PXIE, 0xFFFFFFFFU);
            writel(AHCI_GHC_AE | AHCI_GHC_IE,
                   ahci_reg(port->regs, AHCI_GHC));
        } else {
            printf("[AHCI] failed to register IRQ %d; polling enabled\n",
                   irq);
        }
    }

    port->block.read_sector = ahci_block_read;
    port->block.write_sector = ahci_block_write;
    port->block.capacity = port->capacity;
    port->block.sector_size = AHCI_SECTOR_SIZE;
    port->block.priv = port;
    dev->drv_priv = port;
    g_ahci_ready = 1;
    printf("[AHCI] device on port %u, capacity=%lu sectors\n", port->port_no,
           (unsigned long)port->capacity);
    return 0;

fail:
    (void)ahci_stop_port(port);
    if (port->transfer) dma_free_coherent(port->transfer, AHCI_TRANSFER_BYTES, port->transfer_dma);
    if (port->tables) dma_free_coherent(port->tables, AHCI_CMD_SLOTS * sizeof(*port->tables), port->tables_dma);
    if (port->rfis) dma_free_coherent(port->rfis, 256U, port->rfis_dma);
    if (port->cmd_list) dma_free_coherent(port->cmd_list, 1024U, port->cmd_list_dma);
    memset(port, 0, sizeof(*port));
    return ret;
}

static int ahci_remove(device_t *dev) {
    ahci_port_t *port = dev ? (ahci_port_t *)dev->drv_priv : NULL;
    if (!port)
        return 0;
    g_ahci_ready = 0;
    /* Mask device interrupts before releasing the handler so a completion
     * racing the remove cannot wake a torn-down port. */
    ahci_write(port, AHCI_PXIE, 0);
    if (port->irq_registered)
        free_irq((uint32_t)port->irq, port);
    (void)ahci_stop_port(port);
    if (port->transfer) dma_free_coherent(port->transfer, AHCI_TRANSFER_BYTES, port->transfer_dma);
    if (port->tables) dma_free_coherent(port->tables, AHCI_CMD_SLOTS * sizeof(*port->tables), port->tables_dma);
    if (port->rfis) dma_free_coherent(port->rfis, 256U, port->rfis_dma);
    if (port->cmd_list) dma_free_coherent(port->cmd_list, 1024U, port->cmd_list_dma);
    dev->drv_priv = NULL;
    memset(port, 0, sizeof(*port));
    return 0;
}

static int ahci_class_read(device_t *dev, uint64_t lba, void *buf, size_t count) {
    ahci_port_t *port = dev ? (ahci_port_t *)dev->drv_priv : NULL;
    return ahci_rw(port, lba, buf, count, 0);
}

static int ahci_class_write(device_t *dev, uint64_t lba, const void *buf, size_t count) {
    ahci_port_t *port = dev ? (ahci_port_t *)dev->drv_priv : NULL;
    return ahci_rw(port, lba, (void *)buf, count, 1);
}

static uint64_t ahci_class_capacity(device_t *dev) {
    ahci_port_t *port = dev ? (ahci_port_t *)dev->drv_priv : NULL;
    return port ? port->capacity : 0;
}

static uint32_t ahci_class_sector_size(device_t *dev) {
    (void)dev;
    return AHCI_SECTOR_SIZE;
}

static const block_dev_ops_t ahci_class_ops = {
    .read = ahci_class_read,
    .write = ahci_class_write,
    .capacity = ahci_class_capacity,
    .sector_size = ahci_class_sector_size,
};

static const device_id_t ahci_ids[] = {
    { .vendor = 0x8086U, .device = 0x2922U, .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = 0x8086U, .device = 0x2829U, .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static driver_t ahci_driver = {
    .name = "ahci",
    .id_table = ahci_ids,
    .bus = &pci_bus,
    .probe = ahci_probe,
    .remove = ahci_remove,
    .class_ops = &ahci_class_ops,
    .class_type = DEV_CLASS_BLOCK,
};

DRIVER_REGISTER(ahci_driver);

#endif /* CONFIG_AHCI */
