#ifdef CONFIG_AHCI

#include "drivers/block/ahci.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/platform_bus.h"
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
#include "core/cpu.h"
#include "core/bootargs.h"
#include "core/perf.h"
#include "proc/proc.h"

#define AHCI_MAX_PORTS          32U
#define AHCI_CMD_SLOTS          32U
#define AHCI_SECTOR_SIZE        512U
#define AHCI_TRANSFER_SECTORS   128U
#define AHCI_TRANSFER_BYTES     (AHCI_TRANSFER_SECTORS * AHCI_SECTOR_SIZE)
#define AHCI_TIMEOUT_MS         5000U

#define AHCI_CAP                0x00U
#define AHCI_GHC                0x04U
#define AHCI_IS                 0x08U
#define AHCI_PI                 0x0CU
#define AHCI_VS                 0x10U
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
#define ATA_CMD_FLUSH_CACHE_EXT 0xE7U

/* Hybrid completion window: with a completion IRQ live the window only has to
 * outlast a command that has already retired, because anything still in flight
 * wakes the parked submitter from ahci_irq_handler().  The old 800us window was
 * sized for a transport that never raises an interrupt, and charged every
 * command on every I/O for it.  The no-ISR fallback below keeps polling for the
 * whole timeout instead, which is where the long window is actually needed. */
#define AHCI_HYBRID_PRE_POLL_US 50U
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
    uintptr_t host_regs;
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
    int read_only;
    /* AHCI_IRQ_MODEL:
     * - The IRQ top-half write-clears PxIS and records the bits it
     *   consumed in last_is, because the waiter must still observe TFES
     *   after the hardware status register has been acknowledged.
     * - last_is crosses the IRQ boundary, so it is read and written with
     *   __atomic acquire/release.  `volatile` alone would order nothing on a
     *   weakly ordered ISA, and the window is real: the handler can publish
     *   last_is and wake the submitter while the submitter is still in
     *   wait_queue_link().
     * - Parked submitters sleep on waiters; the handler collects them
     *   after recording last_is.  Only slot 0 is ever in flight (the
     *   port mutex serializes commands), so one status word suffices. */
    uint32_t last_is;
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

/*
 * a20.ahci.poll=1 forces the completion path to stay in bounded polling even
 * when the controller has a usable interrupt line.  Absent or =0 is the
 * interrupt path, which is the default because it is the one that does not
 * charge every command a hardware round trip.
 *
 * This is a kernel parameter rather than a build flag on purpose: whether a
 * machine's PHY/route raises a usable interrupt is a property of that
 * machine, and one image is expected to run on all of them.  The knob is also
 * the only way to keep the polling fallback covered by a test -- a fallback
 * nothing ever selects is a fallback nobody knows still works.
 */
static int ahci_poll_requested(void)
{
    const char *cmdline = bootargs_get();
    static const char key[] = "a20.ahci.poll=";
    /* memcmp(), not strncmp(): this file is also #included verbatim by
     * kernel/drvmod/examples/ahci.c, and a loadable module may only call
     * symbols in the kernel's drv_export_table[] -- strncpy/memcmp/strcmp/
     * strlen are exported (kernel/drvmod/framework.c:313-318) but strncmp is
     * not, and the loader rejects the package outright on an unresolved
     * symbol (kernel/drvmod/loader.c:236-243) rather than failing later at
     * run time.  strncmp() also stops at the token's NUL, so it never read
     * past tok_end on a short token; memcmp() would, hence the explicit
     * length check that stands in for that early stop. */
    const char *p = cmdline;
    while (p && *p) {
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if ((size_t)(p - tok) >= sizeof(key) - 1 &&
            memcmp(tok, key, sizeof(key) - 1) == 0)
            return tok[sizeof(key) - 1] >= '1' && tok[sizeof(key) - 1] <= '9';
        while (*p == ' ' || *p == '\t')
            p++;
    }
    return 0;
}

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

static int ahci_wait_ready(ahci_port_t *port) {
    for (unsigned ms = 0; ms < AHCI_TIMEOUT_MS; ms++) {
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

static int ahci_wait_complete(ahci_port_t *port, size_t bytes);

/*
 * Ring the doorbell for the one command slot this driver uses.
 *
 * PxCI is the only thing that makes the HBA act: until slot 0's bit is set,
 * the command header and table the submitter just filled are inert memory,
 * the HBA never fetches them, and the drive never sees the FIS.  Both
 * submit paths below have to end here, and the callers must have pushed the
 * header/table/PRDT to the device first -- the doorbell is a plain volatile
 * MMIO store, so it must come last for the fetch to see written memory.
 *
 * Omitting it was invisible while ahci.c was compile-verified only: with
 * PxCI still 0, ahci_wait_complete()'s "nothing outstanding" test was true on
 * its very first read and every command reported success for work the
 * controller had never started.
 */
static void ahci_issue_slot0(ahci_port_t *port) {
    ahci_write(port, AHCI_PXCI, 1U);
}

static int ahci_submit(ahci_port_t *port, uint8_t command, uint64_t lba,
                       uint16_t sectors, int write, uint64_t dma, size_t bytes) {
    if (port->read_only && (write || command == ATA_CMD_WRITE_DMA_EXT))
        return -EROFS;
    if (!sectors || bytes > AHCI_TRANSFER_BYTES)
        return -EINVAL;
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
    ahci_issue_slot0(port);
    a20_perf_count(A20_PERF_AHCI_COMMANDS);
    return ahci_wait_complete(port, bytes);
}

/*
 * Issue a command that transfers no data.  FLUSH CACHE EXT is the only one
 * A20OS sends, and it is what makes fsync() durable on AHCI: a preceding
 * WRITE DMA EXT has only reached the drive's volatile cache, which a power
 * cut can still discard.
 *
 * LBA is set to 0xFFFFFFFF with count 0, which ACS-4 defines as "flush the
 * entire cache"; a port flush is inherently device-wide, so there is no
 * narrower form to expose.  PRDTL is 0 because there is no data buffer, and
 * CFLAGS bit 6 (Write) stays clear.
 */
static int ahci_submit_nodata(ahci_port_t *port, uint8_t command) {
    if (port->read_only)
        return -EROFS;
    if (ahci_wait_ready(port) != 0)
        return -1;

    ahci_cmd_header_t *header = &port->cmd_list[0];
    ahci_cmd_table_t *table = &port->tables[0];
    memset(header, 0, sizeof(*header));
    memset(table, 0, sizeof(*table));

    header->flags = 5U;   /* CFL=5 dwords (command FIS), no data direction */
    header->prdtl = 0;   /* no PRD entries: this command moves no data */
    header->ctba = (uint32_t)port->tables_dma;
    header->ctbau = (uint32_t)(port->tables_dma >> 32);

    table->cfis[0] = 0x27U;
    table->cfis[1] = 0x80U;
    table->cfis[2] = command;
    table->cfis[3] = 0x00U;                 /* features */
    table->cfis[4] = 0xFFU;                 /* LBA low  = 0xFFFFFFFF */
    table->cfis[5] = 0xFFU;                 /* LBA mid */
    table->cfis[6] = 0xFFU;                 /* LBA high */
    table->cfis[7] = 0x40U;                 /* device: LBA mode */
    table->cfis[8] = 0xFFU;                 /* LBA low  extended */
    table->cfis[9] = 0xFFU;                 /* LBA mid  extended */
    table->cfis[10] = 0xFFU;                /* LBA high extended */
    table->cfis[12] = 0x00U;                /* count low  = 0: flush all */
    table->cfis[13] = 0x00U;                /* count high */

    dma_sync_for_device(port->cmd_list, 1024U);
    dma_sync_for_device(table, sizeof(*table));
    ahci_issue_slot0(port);
    a20_perf_count(A20_PERF_AHCI_COMMANDS);
    return ahci_wait_complete(port, 0);
}

/*
 * Wait for the command currently in flight to retire.  Shared by the data
 * commands and the no-data commands (FLUSH CACHE) so both observe the same
 * timeout, hybrid poll/park behaviour and TFES error handling.
 */
/*
 * Bounded pause between register re-reads.  udelay() re-reads the board timer
 * through current_board->timer->read_ticks() -- two indirect calls per
 * iteration -- and carries no barrier, so a tight loop around it neither yields
 * the pipeline nor stops the compiler from hoisting the MMIO reads it is
 * supposed to be spacing out.  cpu_relax() plus an explicit clobber is what
 * makes this a poll rather than a speculative spin.  udelay() itself is
 * declared in driver_hwapi.c and is shared with every other driver, so the fix
 * belongs there rather than here.
 */
static void ahci_poll_pause(uint64_t usecs)
{
    uint64_t wait = US_TO_TICKS(usecs);
    uint64_t start = timer_get_ticks();
    while ((timer_get_ticks() - start) < wait) {
        cpu_relax();
        __asm__ __volatile__("" ::: "memory");
    }
}

static int ahci_wait_complete(ahci_port_t *port, size_t bytes) {
    uint64_t start = timer_get_ticks();
    uint64_t deadline = start + MS_TO_TICKS(AHCI_TIMEOUT_MS);
    uint64_t pre_poll_until = start + US_TO_TICKS(AHCI_HYBRID_PRE_POLL_US);

    for (;;) {
        uint32_t is = ahci_read(port, AHCI_PXIS) |
            __atomic_load_n(&port->last_is, __ATOMIC_ACQUIRE);
        if (is & AHCI_PXIS_TFES) {
            ahci_write(port, AHCI_PXIS, 0xFFFFFFFFU);
            __atomic_store_n(&port->last_is, 0, __ATOMIC_RELEASE);
            a20_perf_count(A20_PERF_AHCI_ERRORS);
            return -1;
        }
        if ((ahci_read(port, AHCI_PXCI) & 1U) == 0) {
            __atomic_store_n(&port->last_is, 0, __ATOMIC_RELEASE);
            if (bytes)
                dma_sync_for_cpu(port->transfer, bytes);
            if (!port->irq_registered)
                a20_perf_count(A20_PERF_AHCI_POLL_COMPLETIONS);
            return 0;
        }
        if (!port->irq_registered) {
            if (timer_get_ticks() >= deadline)
                return -1;
            ahci_poll_pause(1000);
            continue;
        }
        uint64_t now = timer_get_ticks();
        if (now >= deadline)
            return -1;
        if (now < pre_poll_until) {
            ahci_poll_pause(20);
            continue;
        }
        /* Park until the completion IRQ; the bounded chunk turns a
         * hypothetical missed wake into a re-check instead of a stall. */
        a20_perf_count(A20_PERF_AHCI_PARK_ROUNDS);
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
            (__atomic_load_n(&port->last_is, __ATOMIC_ACQUIRE) &
             AHCI_PXIS_TFES)) {
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
    if (port && port->read_only && write)
        return -EROFS;
    if (!port || !buf || !count || lba >= port->capacity || count > port->capacity - lba)
        return -EINVAL;
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

static int ahci_block_flush(block_dev_t *dev) {
    ahci_port_t *port = (ahci_port_t *)dev->priv;
    if (!port)
        return -ENODEV;
    /* Serialised against ahci_rw: FLUSH CACHE is a whole-device operation, so
     * it must not interleave with a multi-sector write on the same port. */
    mutex_lock(&port->lock);
    int r = ahci_submit_nodata(port, ATA_CMD_FLUSH_CACHE_EXT);
    mutex_unlock(&port->lock);
    return r == 0 ? 0 : -EIO;
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
     * last_is before the write-clear keeps TFES observable to the waiter.
     *
     * The release store is what makes the recorded bits visible to a waiter
     * that is woken immediately below: on a weakly ordered ISA the write to
     * last_is and the subsequent proc_wake_q_flush() are otherwise free to
     * reach the woken CPU in the other order, and the waiter would re-park
     * for a completion that already happened.  It is also free to deadlock in
     * the other direction, which is why the waiter re-checks after linking.
     */
    uint32_t is = ahci_read(port, AHCI_PXIS);
    if (is) {
        uint32_t merged = __atomic_load_n(&port->last_is, __ATOMIC_RELAXED) | is;
        __atomic_store_n(&port->last_is, merged, __ATOMIC_RELEASE);
        ahci_write(port, AHCI_PXIS, is);
        a20_perf_count(A20_PERF_AHCI_IRQ_COMPLETIONS);
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        unsigned woken = wait_queue_collect_all(&port->waiters, 0,
                                                PROC_WAKE_EVENT, &wake_q,
                                                NULL);
        a20_perf_add(A20_PERF_AHCI_IRQ_WAKEUPS, woken);
        (void)proc_wake_q_flush(&wake_q);
    }
    return 0;
}

static int ahci_wait_link(ahci_port_t *port) {
    uint32_t ssts = 0;
    for (unsigned ms = 0; ms < AHCI_TIMEOUT_MS; ms++) {
        ssts = ahci_read(port, AHCI_PXSSTS);
        if ((ssts & 0x0FU) == 3U)
            return 1;
        mdelay(1);
    }
    /* Task-file status is not reliable until the command engine and receive
     * FIS area are active.  At this stage only use the PHY's DET field; the
     * bounded IDENTIFY path validates command readiness after port start. */
    return 0;
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

static int ahci_probe_common(device_t *dev, int irq, uint32_t flags,
                             uint32_t port_map) {
    int ret = 0;
    if (g_ahci_ready)
        return -ENODEV;

    resource_t *abar = device_get_resource(dev, RES_MMIO, 0);
    if (!abar)
        return -ENODEV;

    ahci_port_t *port = &g_ahci_port;
    memset(port, 0, sizeof(*port));
    port->host_regs = (uintptr_t)abar->start;
    port->regs = port->host_regs;
    port->irq = -1;
    port->read_only = !!(flags & AHCI_PLATFORM_F_READ_ONLY);
    int preserve_firmware_link =
        !!(flags & AHCI_PLATFORM_F_PRESERVE_FIRMWARE_LINK);
    int force_poll = ahci_poll_requested();
    mutex_init(&port->lock);
    wait_queue_init(&port->waiters);

    uint32_t cap = readl(ahci_reg(port->host_regs, AHCI_CAP));
    uint32_t version = readl(ahci_reg(port->host_regs, AHCI_VS));
    uint32_t ghc = readl(ahci_reg(port->host_regs, AHCI_GHC));
    if (preserve_firmware_link) {
        /* Some platform PHYs lose link across a generic HBA reset.  Preserve
         * the firmware-established state while taking ownership, but keep
         * host interrupts disabled for the polling-only handoff. */
        writel((ghc | AHCI_GHC_AE) & ~(AHCI_GHC_IE | AHCI_GHC_HR),
               ahci_reg(port->host_regs, AHCI_GHC));
    } else {
        writel(ghc | AHCI_GHC_AE | AHCI_GHC_HR,
               ahci_reg(port->host_regs, AHCI_GHC));
        for (unsigned ms = 0; ms < AHCI_TIMEOUT_MS; ms++) {
            if ((readl(ahci_reg(port->host_regs, AHCI_GHC)) &
                 AHCI_GHC_HR) == 0)
                break;
            mdelay(1);
            if (ms + 1U == AHCI_TIMEOUT_MS)
                return -ETIMEDOUT;
        }
        writel(AHCI_GHC_AE, ahci_reg(port->host_regs, AHCI_GHC));
    }

    uint32_t pi = readl(ahci_reg(port->host_regs, AHCI_PI));
    if (port_map) {
        writel(pi | port_map, ahci_reg(port->host_regs, AHCI_PI));
        pi = readl(ahci_reg(port->host_regs, AHCI_PI));
    }
    unsigned ports = 0;
    for (uint32_t n = 0; n < AHCI_MAX_PORTS; n++)
        if (pi & (1U << n))
            ports++;
    printf("[AHCI] controller version=%x cap=%x ports=%u mode=%s\n",
           version, cap, ports, port->read_only ? "read-only" : "read-write");

    port->port_no = AHCI_MAX_PORTS;
    for (uint32_t n = 0; n < AHCI_MAX_PORTS; n++) {
        if (!(pi & (1U << n)))
            continue;
        port->port_no = n;
        port->regs = (uintptr_t)abar->start + AHCI_PORT_BASE + n * AHCI_PORT_STRIDE;
        if (preserve_firmware_link) {
            printf("[AHCI] port %u firmware state ssts=%x sctl=%x tfd=%x\n",
                   n, ahci_read(port, AHCI_PXSSTS),
                   ahci_read(port, AHCI_PXSCTL),
                   ahci_read(port, AHCI_PXTFD));
        } else {
            ahci_write(port, AHCI_PXSCTL, 0x301U);
            mdelay(1);
            ahci_write(port, AHCI_PXSCTL, 0x300U);
        }
        if (ahci_wait_link(port))
            break;
        printf("[AHCI] port %u unavailable ssts=%x tfd=%x\n", n,
               ahci_read(port, AHCI_PXSSTS), ahci_read(port, AHCI_PXTFD));
        port->port_no = AHCI_MAX_PORTS;
    }
    if (port->port_no == AHCI_MAX_PORTS) {
        printf("[AHCI] no ready SATA port found\n");
        return -ENODEV;
    }

    port->cmd_list = dma_alloc_coherent_aligned(dev, 1024U, 1024U,
                                                &port->cmd_list_dma);
    port->rfis = dma_alloc_coherent_aligned(dev, 256U, 256U, &port->rfis_dma);
    port->tables = dma_alloc_coherent_aligned(dev,
        AHCI_CMD_SLOTS * sizeof(*port->tables), 128U, &port->tables_dma);
    port->transfer = dma_alloc_coherent_aligned(dev,
        AHCI_TRANSFER_BYTES, AHCI_SECTOR_SIZE, &port->transfer_dma);
    /* A narrowed mask the board cannot satisfy shows up as NULL here, and the
     * same -ENOMEM the allocation failure reports: the port must not come up
     * holding lists the HBA cannot address. */
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

    /* Clear whatever IDENTIFY left behind before the handler exists, so the
     * first unmask cannot deliver a status bit whose only reader is gone. */
    ahci_write(port, AHCI_PXIS, 0xFFFFFFFFU);
    __atomic_store_n(&port->last_is, 0, __ATOMIC_RELEASE);

    if (force_poll) {
        printf("[AHCI] a20.ahci.poll=1; completion path stays in polling\n");
    } else if (irq >= 0) {
        if (request_irq((uint32_t)irq, ahci_irq_handler, IRQF_SHARED,
                        port) == 0) {
            port->irq = irq;
            port->irq_registered = 1;
            /* Unmask the port and the host only with the handler in place.
             * PxIE stays 0 until here: an HBA asserting a shared level line
             * with nobody clearing its source storms the CPU.  Enabling every
             * port bit (DPS/DSE/PRDIE/TF...) rather than a narrow set is what
             * makes the wait independent of which bit a given HBA raises for
             * which command. */
            ahci_write(port, AHCI_PXIE, 0xFFFFFFFFU);
            writel(AHCI_GHC_AE | AHCI_GHC_IE,
                   ahci_reg(port->host_regs, AHCI_GHC));
        } else {
            printf("[AHCI] failed to register IRQ %d; polling enabled\n",
                   irq);
        }
    }

    port->block.read_sector = ahci_block_read;
    port->block.write_sector = ahci_block_write;
    port->block.flush = ahci_block_flush;
    port->block.capacity = port->capacity;
    port->block.sector_size = AHCI_SECTOR_SIZE;
    port->block.priv = port;
    dev->drv_priv = port;
    g_ahci_ready = 1;
    printf("[AHCI] device on port %u, capacity=%lu sectors%s completion=%s\n",
           port->port_no, (unsigned long)port->capacity,
           port->read_only ? ", writes blocked" : "",
           port->irq_registered ? "irq" : "poll");
    return 0;

fail:
    (void)ahci_stop_port(port);
    if (port->transfer) dma_free_coherent_aligned(port->transfer, AHCI_TRANSFER_BYTES, port->transfer_dma);
    if (port->tables) dma_free_coherent_aligned(port->tables, AHCI_CMD_SLOTS * sizeof(*port->tables), port->tables_dma);
    if (port->rfis) dma_free_coherent_aligned(port->rfis, 256U, port->rfis_dma);
    if (port->cmd_list) dma_free_coherent_aligned(port->cmd_list, 1024U, port->cmd_list_dma);
    memset(port, 0, sizeof(*port));
    return ret;
}

static int ahci_pci_probe(device_t *dev) {
    if (pci_enable_and_assign_bars(dev) < 0)
        return -ENODEV;
    uint32_t flags = 0;
#ifdef CONFIG_STORAGE_READ_ONLY
    flags |= AHCI_PLATFORM_F_READ_ONLY;
#endif
    return ahci_probe_common(dev, pci_intx_irq(dev), flags, 0);
}

static int ahci_platform_probe(device_t *dev) {
    const ahci_platform_data_t *data = dev ?
        (const ahci_platform_data_t *)dev->plat_data : NULL;
    resource_t *irq_res = device_get_resource(dev, RES_IRQ, 0);
    int irq = irq_res ? (int)irq_res->start : -1;
    return ahci_probe_common(dev, irq, data ? data->flags : 0,
                             data ? data->port_map : 0);
}

static int ahci_remove(device_t *dev) {
    ahci_port_t *port = dev ? (ahci_port_t *)dev->drv_priv : NULL;
    if (!port)
        return 0;
    g_ahci_ready = 0;
    /* Mask device interrupts before releasing the handler so a completion
     * racing the remove cannot wake a torn-down port.  Dropping GHC.IE
     * afterwards takes the HBA off the host interrupt line entirely: on a
     * shared INTx route a controller left asserting with its source masked
     * keeps the line asserted for whoever shares that vector. */
    ahci_write(port, AHCI_PXIE, 0);
    if (port->irq_registered) {
        free_irq((uint32_t)port->irq, port);
        writel(AHCI_GHC_AE,
               ahci_reg(port->host_regs, AHCI_GHC));
    }
    (void)ahci_stop_port(port);
    if (port->transfer) dma_free_coherent_aligned(port->transfer, AHCI_TRANSFER_BYTES, port->transfer_dma);
    if (port->tables) dma_free_coherent_aligned(port->tables, AHCI_CMD_SLOTS * sizeof(*port->tables), port->tables_dma);
    if (port->rfis) dma_free_coherent_aligned(port->rfis, 256U, port->rfis_dma);
    if (port->cmd_list) dma_free_coherent_aligned(port->cmd_list, 1024U, port->cmd_list_dma);
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

static int ahci_class_flush(device_t *dev) {
    ahci_port_t *port = dev ? (ahci_port_t *)dev->drv_priv : NULL;
    if (!port)
        return -ENODEV;
    mutex_lock(&port->lock);
    int r = ahci_submit_nodata(port, ATA_CMD_FLUSH_CACHE_EXT);
    mutex_unlock(&port->lock);
    return r == 0 ? 0 : -EIO;
}

static const block_dev_ops_t ahci_class_ops = {
    .read = ahci_class_read,
    .write = ahci_class_write,
    .flush = ahci_class_flush,
    .capacity = ahci_class_capacity,
    .sector_size = ahci_class_sector_size,
};

static const device_id_t ahci_ids[] = {
    { .vendor = 0x8086U, .device = 0x2922U, .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = 0x8086U, .device = 0x2829U, .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static const device_id_t ahci_platform_ids[] = {
    { .vendor = AHCI_PLATFORM_VENDOR, .device = AHCI_PLATFORM_DEVICE,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static driver_t ahci_driver = {
    .name = "ahci",
    .id_table = ahci_ids,
    .bus = &pci_bus,
    .probe = ahci_pci_probe,
    .remove = ahci_remove,
    .class_ops = &ahci_class_ops,
    .class_type = DEV_CLASS_BLOCK,
};

static driver_t ahci_platform_driver = {
    .name = "ahci-platform",
    .id_table = ahci_platform_ids,
    .bus = &platform_bus,
    .probe = ahci_platform_probe,
    .remove = ahci_remove,
    .class_ops = &ahci_class_ops,
    .class_type = DEV_CLASS_BLOCK,
};

DRIVER_REGISTER(ahci_driver);
DRIVER_REGISTER(ahci_platform_driver);

#endif /* CONFIG_AHCI */
