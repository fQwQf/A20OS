#include "drivers/block/virtio_scsi.h"
#include "drivers/block/virtio_blk.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "core/bootargs.h"
#include "core/defs.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/sync.h"
#include "core/timer.h"
#include "mm/mm.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

#define VIRTIO_SCSI_QUEUE_SIZE 32
#define VIRTIO_SCSI_REQUEST_QUEUE 2U
#define VIRTIO_SCSI_MAX_DEVS 4
#define VIRTIO_SCSI_CDB_SIZE 32
#define VIRTIO_SCSI_SENSE_SIZE 96
#define VIRTIO_SCSI_TIMEOUT_TICKS (clock_ticks_per_sec() * 10)
#define VIRTIO_SCSI_POLL_LIMIT    50000000U
/* Hybrid completion window: bounded spin-poll before parking on the
 * completion IRQ, mirroring the virtio-blk model. */
#define VIRTIO_SCSI_HYBRID_PRE_POLL_US 800U
/* Bounded park chunk: a hypothetical missed wake degrades to a re-check. */
#define VIRTIO_SCSI_PARK_CHUNK_MS 50U
/* Queues that want their own message-signalled vector.  Only the request
 * queue carries data-plane completions; the control and event queues are
 * set up but this driver never drives them, so asking for their vectors
 * would arm table entries with no queue behind them. */
#define VIRTIO_SCSI_MSIX_VECTORS 1U

#define SCSI_CMD_TEST_UNIT_READY    0x00U
#define SCSI_CMD_INQUIRY            0x12U
#define SCSI_CMD_READ_CAPACITY10    0x25U
#define SCSI_CMD_READ10             0x28U
#define SCSI_CMD_WRITE10            0x2AU
/* SBC-3 5.32: a SYNCHRONIZE CACHE(10) whose LBNUM and block count are both
 * zero synchronises the whole medium, which is what a flush means here. */
#define SCSI_CMD_SYNC_CACHE10       0x35U
#define SCSI_STATUS_GOOD            0x00U
#define SCSI_STATUS_CHECK_CONDITION 0x02U
#define VIRTIO_SCSI_S_OK            0x00U
/* SPC-4 4.4: sense keys, in the fixed-format sense buffer. */
#define SCSI_SENSE_KEY_UNIT_ATTENTION 0x06U
/* Fixed format: byte 2 carries the key, byte 0 is the 0x70 response code;
 * descriptor format: byte 1 carries the key behind the 0x72 code.  QEMU
 * auto-converts to fixed for an HBA that does not ask (SAM-5 4.4.2). */
#define SCSI_FIXED_SENSE_KEY(byte)   ((byte)[2] & 0x0fU)
#define SCSI_FIXED_SENSE_IS_FIXED(sense) (((sense)[0] & 0x02U) == 0U)
#define SCSI_DESCRIPTOR_SENSE_KEY(byte) ((byte)[1] & 0x0fU)
/* A unit attention is cleared by being reported, so one retry is normally
 * enough; the bound keeps a medium that keeps re-arming the condition from
 * turning a single command into an unbounded loop. */
#define VIRTIO_SCSI_UA_RETRIES 4U

typedef struct {
    uint8_t lun[8];
    uint64_t tag;
    uint8_t task_attr;
    uint8_t prio;
    uint8_t crn;
    uint8_t cdb[VIRTIO_SCSI_CDB_SIZE];
} __attribute__((packed)) virtio_scsi_req_t;

typedef struct {
    uint32_t sense_len;
    uint32_t resid;
    uint16_t status_qualifier;
    uint8_t status;
    uint8_t response;
    uint8_t sense[VIRTIO_SCSI_SENSE_SIZE];
} __attribute__((packed)) virtio_scsi_resp_t;

typedef struct {
    virtq_desc_t desc[VIRTIO_SCSI_QUEUE_SIZE] ALIGNED(64);
    virtq_avail_t avail ALIGNED(64);
    virtq_used_t used ALIGNED(64);
} virtio_scsi_aux_queue_t;

typedef struct {
    virtio_transport_t vt;
    virtq_desc_t desc[VIRTIO_SCSI_QUEUE_SIZE] ALIGNED(64);
    virtq_avail_t avail ALIGNED(64);
    virtq_used_t used ALIGNED(64);
    virtio_scsi_req_t req ALIGNED(64);
    virtio_scsi_resp_t resp ALIGNED(64);
    virtio_scsi_aux_queue_t control;
    virtio_scsi_aux_queue_t event;
    /* Sleepable mutex: the IRQ top-half never takes it, and the command
     * wait path parks while holding it. */
    mutex_t lock;
    /* Parked command waiters woken by the completion IRQ. */
    wait_queue_t waiters;
    int irq_registered;
    /* Which delivery path probe settled on: A20_BLK_IRQ_*.  Reported through
     * the class stats ioctl so "asked for an interrupt and fell back to
     * polling" is distinguishable from "never asked". */
    int irq_mode;
    /* Set once by the first MSI-X completion, so the arrival can be logged
     * once per controller rather than once per command. */
    int msix_first_irq;
    /* Completion accounting.  irq_count is bumped by the top-half only, which
     * is what makes it evidence that the device raised the line rather than
     * something a polling fallback could have produced. */
    uint64_t commands;
    uint64_t flushes;
    uint64_t timeouts;
    uint64_t irq_count;
    uint64_t irq_completions;
    uint64_t spin_completions;
    block_dev_t block;
    uint16_t last_used;
    uint64_t capacity;
    int slot;
    int ready;
} virtio_scsi_dev_t;

static virtio_scsi_dev_t g_scsi[VIRTIO_SCSI_MAX_DEVS];
static int g_scsi_count;

/* a20.virtio-scsi.poll=1 forces the polling completion path even on a
 * transport that exposes an interrupt line.  It is the manual counterpart of
 * an IRQ registration failure, and it exists so a machine whose interrupt
 * delivery is suspect can be shown to still work: with the same binary, one
 * boot reports irq_count rising and the other reports zero. */
static int virtio_scsi_force_poll(void) {
    const char *args = bootargs_get();
    return args && strstr(args, "a20.virtio-scsi.poll=1") != NULL;
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static int virtio_scsi_setup_queue(virtio_scsi_dev_t *dev, uint32_t queue,
                                   virtq_desc_t *desc, virtq_avail_t *avail,
                                   virtq_used_t *used) {
    virtio_transport_t *vt = &dev->vt;
    vt->write32(vt, VIRTIO_MMIO_QUEUE_SEL, queue);
    uint32_t queue_max = vt->read32(vt, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (queue_max < VIRTIO_SCSI_QUEUE_SIZE ||
        vt->read32(vt, VIRTIO_MMIO_QUEUE_READY)) {
        kerr("[VIRTIO-SCSI] queue %u unavailable (max=%u ready=%u)\n",
             queue, queue_max, vt->read32(vt, VIRTIO_MMIO_QUEUE_READY));
        return -1;
    }
    vt->write32(vt, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_SCSI_QUEUE_SIZE);
    memset(desc, 0, sizeof(virtq_desc_t) * VIRTIO_SCSI_QUEUE_SIZE);
    memset(avail, 0, sizeof(*avail));
    memset(used, 0, sizeof(*used));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)va_to_pa(desc));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DESC_HIGH,
                (uint32_t)((uint64_t)va_to_pa(desc) >> 32));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DRIVER_LOW, (uint32_t)va_to_pa(avail));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DRIVER_HIGH,
                (uint32_t)((uint64_t)va_to_pa(avail) >> 32));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DEVICE_LOW, (uint32_t)va_to_pa(used));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DEVICE_HIGH,
                (uint32_t)((uint64_t)va_to_pa(used) >> 32));
    wmb();
    vt->write32(vt, VIRTIO_MMIO_QUEUE_READY, 1);
    return 0;
}

static int virtio_scsi_setup_queues(virtio_scsi_dev_t *dev) {
    if (virtio_scsi_setup_queue(dev, 0, dev->control.desc, &dev->control.avail,
                                &dev->control.used) != 0 ||
        virtio_scsi_setup_queue(dev, 1, dev->event.desc, &dev->event.avail,
                                &dev->event.used) != 0 ||
        virtio_scsi_setup_queue(dev, VIRTIO_SCSI_REQUEST_QUEUE, dev->desc,
                                &dev->avail, &dev->used) != 0)
        return -1;
    return 0;
}

/* VIRTIO_SCSI_IRQ_MODEL:
 * - The top-half acknowledges the device interrupt (ISR read + ack write)
 *   and wakes the parked command issuer; it never touches queue state —
 *   the issuer owns last_used and the single outstanding request buffer.
 * - A spurious or shared-line invocation degrades to one no-op wake.
 * - The wake is unconditional.  Under MSI-X the used-buffer notification
 *   arrives as a message and the ISR register may legitimately read back
 *   zero, so "no ISR bit" must not be read as "no completion"; returning
 *   early there would leave the parked issuer asleep until its park chunk
 *   expires, which is a stall the used ring never caused.
 * - The counters are the only state the handler owns: irq_count is bumped
 *   with a relaxed atomic because nothing else reads it under dev->lock and
 *   the IRQ top-half never takes that lock. */
static int virtio_scsi_irq_handler(int irq, void *priv) {
    (void)irq;
    virtio_scsi_dev_t *dev = (virtio_scsi_dev_t *)priv;
    if (!dev)
        return 0;
    __atomic_fetch_add(&dev->irq_count, 1, __ATOMIC_RELAXED);
    /* One line per controller, the first time a message-signalled completion
     * arrives: the capability being programmed is not the same observation as
     * the device posting a message the platform took. */
    if (dev->irq_mode == A20_BLK_IRQ_MSIX &&
        __atomic_exchange_n(&dev->msix_first_irq, 1, __ATOMIC_RELAXED) == 0)
        kinfo("[VIRTIO-SCSI] MSI-X delivery on vector %d\n", irq);
    uint32_t isr = dev->vt.read32(&dev->vt, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (isr && !dev->vt.legacy)
        dev->vt.write32(&dev->vt, VIRTIO_MMIO_INTERRUPT_ACK, isr);
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    (void)wait_queue_collect_all(&dev->waiters, 0, PROC_WAKE_EVENT,
                                 &wake_q, NULL);
    (void)proc_wake_q_flush(&wake_q);
    return 0;
}

/* Whether this completion was a unit attention: the command reached the
 * medium and was refused only because something else changed first.  That is
 * a "retry the same command", not a failure -- SAM-5 4.4 and SCSI's own
 * ready-state rules, and what Linux does in scsi_check_ready_state().  The
 * condition is cleared by being reported, so the retry normally succeeds.
 * Called under dev->lock: it reads the shared response buffer. */
static int virtio_scsi_was_unit_attention(const virtio_scsi_dev_t *dev) {
    if (dev->resp.response != VIRTIO_SCSI_S_OK ||
        dev->resp.status != SCSI_STATUS_CHECK_CONDITION ||
        dev->resp.sense_len < 3U)
        return 0;
    const uint8_t *sense = dev->resp.sense;
    uint8_t key = SCSI_FIXED_SENSE_IS_FIXED(sense)
                      ? SCSI_FIXED_SENSE_KEY(sense)
                      : SCSI_DESCRIPTOR_SENSE_KEY(sense);
    return key == SCSI_SENSE_KEY_UNIT_ATTENTION;
}

/* *unit_attention is the verdict for this attempt, sampled while the lock is
 * still held, so the caller does not have to re-read dev->resp to decide
 * whether the command is worth retrying. */
static int virtio_scsi_command_once(virtio_scsi_dev_t *dev, const uint8_t *cdb,
                                    void *data, size_t bytes, int data_in,
                                    int *unit_attention) {
    mutex_lock(&dev->lock);
    *unit_attention = 0;
    dev->commands++;
    memset(&dev->req, 0, sizeof(dev->req));
    memset(&dev->resp, 0, sizeof(dev->resp));
    memcpy(dev->req.cdb, cdb, VIRTIO_SCSI_CDB_SIZE);
    /* VirtIO-SCSI's simple addressing: format 1, target 0, LUN 0. */
    dev->req.lun[0] = 1;
    dev->req.lun[1] = 0;
    dev->req.tag = (uint64_t)dev->last_used + 1;

    dev->desc[0].addr = va_to_pa(&dev->req);
    dev->desc[0].len = sizeof(dev->req);
    if (bytes && !data_in) {
        /* Device-readable descriptors must precede device-writable ones. */
        dev->desc[0].flags = VIRTQ_DESC_F_NEXT;
        dev->desc[0].next = 1;
        dev->desc[1].addr = va_to_pa(data);
        dev->desc[1].len = (uint32_t)bytes;
        dev->desc[1].flags = VIRTQ_DESC_F_NEXT;
        dev->desc[1].next = 2;
        dev->desc[2].addr = va_to_pa(&dev->resp);
        dev->desc[2].len = sizeof(dev->resp);
        dev->desc[2].flags = VIRTQ_DESC_F_WRITE;
        dev->desc[2].next = 0;
    } else if (bytes) {
        /* VirtIO-SCSI input order is request, response, then data-in.  VBox
         * splits the chain at the first writable descriptor and therefore
         * interprets that first buffer as the response header. */
        dev->desc[0].flags = VIRTQ_DESC_F_NEXT;
        dev->desc[0].next = 2;
        dev->desc[2].addr = va_to_pa(&dev->resp);
        dev->desc[2].len = sizeof(dev->resp);
        dev->desc[2].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;
        dev->desc[2].next = 1;
        dev->desc[1].addr = va_to_pa(data);
        dev->desc[1].len = (uint32_t)bytes;
        dev->desc[1].flags = VIRTQ_DESC_F_WRITE;
        dev->desc[1].next = 0;
    } else {
        dev->desc[0].flags = VIRTQ_DESC_F_NEXT;
        dev->desc[0].next = 2;
        dev->desc[2].addr = va_to_pa(&dev->resp);
        dev->desc[2].len = sizeof(dev->resp);
        dev->desc[2].flags = VIRTQ_DESC_F_WRITE;
        dev->desc[2].next = 0;
    }

    uint16_t used_before = dev->used.idx;
    uint16_t avail_slot = dev->avail.idx % VIRTIO_SCSI_QUEUE_SIZE;
    dev->avail.ring[avail_slot] = 0;
    wmb();
    dev->avail.idx++;
    arch_dma_sync_for_device(&dev->req, sizeof(dev->req));
    if (bytes)
        arch_dma_sync_for_device(data, bytes);
    arch_dma_sync_for_device(&dev->resp, sizeof(dev->resp));
    arch_dma_sync_for_device(dev->desc, sizeof(dev->desc));
    arch_dma_sync_for_device(&dev->avail, sizeof(dev->avail));
    arch_dma_sync_for_device(&dev->used, sizeof(dev->used));
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_QUEUE_NOTIFY, VIRTIO_SCSI_REQUEST_QUEUE);

    uint64_t start = timer_get_ticks();
    uint64_t deadline = start + VIRTIO_SCSI_TIMEOUT_TICKS;
    uint64_t pre_poll_until = start + US_TO_TICKS(VIRTIO_SCSI_HYBRID_PRE_POLL_US);
    uint32_t spins = VIRTIO_SCSI_POLL_LIMIT;
    /* Which side of the pre-poll window the completion landed on.  Both are
     * real completions; the split is what says how much of the interrupt path
     * a given workload actually depends on. */
    int completed_while_parked = 0;
    for (;;) {
        arch_dma_sync_for_cpu(&dev->used, sizeof(dev->used));
        if (dev->used.idx != used_before)
            break;
        /* VirtualBox's early ARM timer is a software fallback.  Bound the
         * poll even if it is not advancing yet, so a failed controller does
         * not freeze the whole boot permanently. */
        if (timer_get_ticks() >= deadline || --spins == 0) {
            dev->timeouts++;
            mutex_unlock(&dev->lock);
            kinfo("[VIRTIO-SCSI] request timeout\n");
            return -1;
        }
        if (!dev->irq_registered || timer_get_ticks() < pre_poll_until) {
            arch_cpu_relax();
            continue;
        }
        /* Park until the completion IRQ; the bounded chunk turns a
         * hypothetical missed wake into a re-check instead of a stall. */
        uint64_t chunk = timer_get_ticks() + MS_TO_TICKS(VIRTIO_SCSI_PARK_CHUNK_MS);
        if (chunk > deadline)
            chunk = deadline;
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, chunk);
        wait_queue_entry_t entry = {0};
        wait_queue_link(&dev->waiters, &entry, token, 0);
        arch_dma_sync_for_cpu(&dev->used, sizeof(dev->used));
        if (dev->used.idx != used_before) {
            wait_queue_unlink(&dev->waiters, &entry);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            completed_while_parked = 1;
            break;
        }
        (void)proc_park_commit(token);
        wait_queue_unlink(&dev->waiters, &entry);
        proc_park_finish(token);
    }
    if (completed_while_parked)
        dev->irq_completions++;
    else
        dev->spin_completions++;
    arch_dma_sync_for_cpu(&dev->resp, sizeof(dev->resp));
    if (bytes)
        arch_dma_sync_for_cpu(data, bytes);
    dev->last_used = dev->used.idx;
    int ok = dev->resp.response == VIRTIO_SCSI_S_OK && dev->resp.status == SCSI_STATUS_GOOD;
    *unit_attention = !ok && virtio_scsi_was_unit_attention(dev);
    /* A unit attention is the caller's to retry, so it is not reported here
     * as a failed command; every other refusal is. */
    if (!ok && !*unit_attention)
        kerr("[VIRTIO-SCSI] SCSI command %02x failed: response=%u status=%u "
             "sense_len=%u resid=%u\n",
             cdb[0], dev->resp.response, dev->resp.status, dev->resp.sense_len,
             dev->resp.resid);
    mutex_unlock(&dev->lock);
    return ok ? 0 : -1;
}

/* Retry a command the medium refused only because a unit attention was
 * pending.  This matters at probe: the driver's own status reset in
 * virtio_scsi_probe() resets the controller, which resets the SCSI bus, which
 * arms "power on, reset or bus device reset occurred" on every disk behind
 * it.  The first TEST UNIT READY after that is required to come back CHECK
 * CONDITION, and a driver that treats that as fatal never brings the disk up. */
static int virtio_scsi_command(virtio_scsi_dev_t *dev, const uint8_t *cdb,
                               void *data, size_t bytes, int data_in) {
    for (unsigned attempt = 0; attempt <= VIRTIO_SCSI_UA_RETRIES; attempt++) {
        int unit_attention = 0;
        if (virtio_scsi_command_once(dev, cdb, data, bytes, data_in,
                                     &unit_attention) == 0)
            return 0;
        if (!unit_attention)
            return -1;
        if (attempt == VIRTIO_SCSI_UA_RETRIES) {
            kerr("[VIRTIO-SCSI] SCSI command %02x: unit attention did not "
                 "clear after %u retries\n", cdb[0], VIRTIO_SCSI_UA_RETRIES);
            return -1;
        }
        kinfo("[VIRTIO-SCSI] SCSI command %02x: unit attention, retrying\n",
              cdb[0]);
    }
    return -1;
}

static int virtio_scsi_rw(block_dev_t *block, uint64_t lba, void *buf,
                          size_t sectors, int write) {
    virtio_scsi_dev_t *dev = (virtio_scsi_dev_t *)block->priv;
    if (!dev || !dev->ready || sectors == 0 || sectors > 0xffffU ||
        lba > 0xffffffffU || lba + sectors > dev->capacity)
        return -1;
    uint8_t cdb[VIRTIO_SCSI_CDB_SIZE] = { 0 };
    cdb[0] = write ? SCSI_CMD_WRITE10 : SCSI_CMD_READ10;
    put_be32(&cdb[2], (uint32_t)lba);
    cdb[7] = (uint8_t)(sectors >> 8);
    cdb[8] = (uint8_t)sectors;
    return virtio_scsi_command(dev, cdb, buf, sectors * 512U, !write);
}

static int virtio_scsi_read(block_dev_t *block, uint64_t lba, void *buf, size_t sectors) {
    return virtio_scsi_rw(block, lba, buf, sectors, 0);
}

static int virtio_scsi_write(block_dev_t *block, uint64_t lba, const void *buf, size_t sectors) {
    return virtio_scsi_rw(block, lba, (void *)buf, sectors, 1);
}

/* SBC-3 SYNCHRONIZE CACHE(10) with LBNUM and block count both zero: the
 * device must push every cached block to the medium.  There is no data
 * descriptor here, so virtio_scsi_command() takes its request/response-only
 * chain.  Reported as unsupported rather than faked when the controller
 * rejects it -- a flush that returns success without having been issued is
 * exactly the failure the filesystem durability path cannot detect. */
static int virtio_scsi_flush(block_dev_t *block) {
    virtio_scsi_dev_t *dev = (virtio_scsi_dev_t *)block->priv;
    if (!dev || !dev->ready)
        return -1;
    uint8_t cdb[VIRTIO_SCSI_CDB_SIZE] = { 0 };
    cdb[0] = SCSI_CMD_SYNC_CACHE10;
    mutex_lock(&dev->lock);
    dev->flushes++;
    mutex_unlock(&dev->lock);
    return virtio_scsi_command(dev, cdb, NULL, 0, 0);
}

static int virtio_scsi_probe(device_t *pdev) {
    if (g_scsi_count >= VIRTIO_SCSI_MAX_DEVS)
        return -1;
    virtio_scsi_dev_t *dev = &g_scsi[g_scsi_count];
    memset(dev, 0, sizeof(*dev));
    dev->vt.irq = -1;
    mutex_init(&dev->lock);
    wait_queue_init(&dev->waiters);
    if (pci_virtio_transport_init(pdev, VIRTIO_ID_SCSI, &dev->vt) != 0) {
        kerr("[VIRTIO-SCSI] PCI transport initialization failed\n");
        return -1;
    }

    dev->vt.write32(&dev->vt, VIRTIO_MMIO_STATUS, 0);
    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_STATUS, status);
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_DRIVER_FEATURES, 0);
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    uint32_t features_hi = dev->vt.read32(&dev->vt, VIRTIO_MMIO_DEVICE_FEATURES);
    if (!(features_hi & VIRTIO_F_VERSION_1_BIT)) {
        kerr("[VIRTIO-SCSI] device lacks VIRTIO_F_VERSION_1\n");
        return -1;
    }
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_DRIVER_FEATURES, VIRTIO_F_VERSION_1_BIT);
    status |= VIRTIO_STATUS_FEATURES_OK;
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_STATUS, status);
    if (!(dev->vt.read32(&dev->vt, VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        kerr("[VIRTIO-SCSI] device rejected feature negotiation\n");
        return -1;
    }
    if (virtio_scsi_setup_queues(dev) != 0)
        return -1;
    status |= VIRTIO_STATUS_DRIVER_OK;
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_STATUS, status);

    /* Completion delivery, best first: a message-signalled vector for the
     * request queue, then the shared INTx line, then polling.  Each step is
     * entered only when the previous one failed, so a device that offers
     * MSI-X never takes the level-triggered line it was trying to get away
     * from.  The order and the shape of each step follow virtio_blk.c. */
    dev->irq_mode = A20_BLK_IRQ_POLL;
    if (virtio_scsi_force_poll()) {
        kinfo("[VIRTIO-SCSI] %s: a20.virtio-scsi.poll=1; using polling\n",
              pdev->name);
        dev->vt.irq = -1;
    } else if (dev->vt.msix_prepare) {
        int r = dev->vt.msix_prepare(&dev->vt, VIRTIO_SCSI_MSIX_VECTORS);
        if (r == 0) {
            int base = dev->vt.msix_base;
            if (request_irq((uint32_t)base, virtio_scsi_irq_handler, 0, dev) == 0) {
                dev->irq_registered = 1;
                dev->irq_mode = A20_BLK_IRQ_MSIX;
                /* The INTx line is no longer this device's to use: leaving a
                 * handler on it would give one device two delivery paths. */
                dev->vt.irq = -1;
                dev->vt.msix_arm(&dev->vt);
                kinfo("[VIRTIO-SCSI] %s using MSI-X vector %d completions\n",
                      pdev->name, base);
            } else {
                dev->vt.msix_teardown(&dev->vt);
                kinfo("[VIRTIO-SCSI] %s MSI-X handler registration failed; "
                      "falling back to the shared INTx line\n", pdev->name);
            }
        } else {
            kinfo("[VIRTIO-SCSI] %s: MSI-X unavailable (%d)\n", pdev->name, r);
        }
    }
    if (!dev->irq_registered && dev->vt.irq >= 0) {
        unsigned long irq_flags = dev->vt.shared_irq ? IRQF_SHARED : 0;
        if (request_irq((uint32_t)dev->vt.irq, virtio_scsi_irq_handler,
                        irq_flags, dev) == 0) {
            dev->irq_registered = 1;
            dev->irq_mode = A20_BLK_IRQ_INTX;
            kinfo("[VIRTIO-SCSI] %s using IRQ %d completions\n",
                  pdev->name, dev->vt.irq);
        } else {
            /* Never park on an interrupt that will not arrive. */
            kinfo("[VIRTIO-SCSI] %s IRQ %d registration failed; using polling\n",
                  pdev->name, dev->vt.irq);
            dev->vt.irq = -1;
        }
    }
    if (!dev->irq_registered)
        kinfo("[VIRTIO-SCSI] %s using completion polling\n", pdev->name);

    uint8_t cdb[VIRTIO_SCSI_CDB_SIZE] = { 0 };
    uint8_t capacity[8] ALIGNED(64) = { 0 };
    cdb[0] = SCSI_CMD_TEST_UNIT_READY;
    if (virtio_scsi_command(dev, cdb, capacity, 0, 1) != 0)
        return -1;
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_CMD_READ_CAPACITY10;
    if (virtio_scsi_command(dev, cdb, capacity, sizeof(capacity), 1) != 0)
        return -1;
    dev->capacity = (uint64_t)be32(capacity) + 1U;
    if (!dev->capacity || be32(capacity + 4) != 512U)
        return -1;
    dev->block.read_sector = virtio_scsi_read;
    dev->block.write_sector = virtio_scsi_write;
    dev->block.flush = virtio_scsi_flush;
    dev->block.capacity = dev->capacity;
    dev->block.sector_size = 512;
    dev->block.priv = dev;
    dev->slot = g_scsi_count;
    dev->ready = 1;
    pdev->drv_priv = dev;
    g_scsi_count++;
    kinfo("[VIRTIO-SCSI] disk ready: %lu sectors (%lu MiB), completion=%s irq=%d\n",
          (unsigned long)dev->capacity, (unsigned long)(dev->capacity / 2048U),
          dev->irq_mode == A20_BLK_IRQ_MSIX ? "msix" :
          dev->irq_mode == A20_BLK_IRQ_INTX ? "intx" : "poll",
          dev->vt.irq);
    return 0;
}

static int virtio_scsi_remove(device_t *pdev) {
    virtio_scsi_dev_t *dev = pdev ? pdev->drv_priv : NULL;
    if (!dev)
        return 0;
    dev->ready = 0;
    /* Device reset stops all interrupt sources before the handler goes. */
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_STATUS, 0);
    mb();
    if (dev->irq_mode == A20_BLK_IRQ_MSIX) {
        /* The vectors live at msix_base, not at dev->vt.irq, which MSI-X
         * registration cleared on purpose. */
        free_irq((uint32_t)dev->vt.msix_base, dev);
        dev->vt.msix_teardown(&dev->vt);
    } else if (dev->irq_registered) {
        free_irq((uint32_t)dev->vt.irq, dev);
    }
    dev->irq_registered = 0;
    dev->irq_mode = A20_BLK_IRQ_POLL;
    pdev->drv_priv = NULL;
    while (g_scsi_count > 0 && !g_scsi[g_scsi_count - 1].ready)
        g_scsi_count--;
    return 0;
}

static uint64_t virtio_scsi_class_capacity(device_t *dev) {
    virtio_scsi_dev_t *scsi = dev ? (virtio_scsi_dev_t *)dev->drv_priv : NULL;
    return scsi ? scsi->capacity : 0;
}

static int virtio_scsi_class_read(device_t *dev, uint64_t sector, void *buf, size_t count) {
    virtio_scsi_dev_t *scsi = dev ? (virtio_scsi_dev_t *)dev->drv_priv : NULL;
    return scsi ? virtio_scsi_read(&scsi->block, sector, buf, count) : -1;
}

static int virtio_scsi_class_write(device_t *dev, uint64_t sector, const void *buf, size_t count) {
    virtio_scsi_dev_t *scsi = dev ? (virtio_scsi_dev_t *)dev->drv_priv : NULL;
    return scsi ? virtio_scsi_write(&scsi->block, sector, buf, count) : -1;
}

static int virtio_scsi_class_flush(device_t *dev) {
    virtio_scsi_dev_t *scsi = dev ? (virtio_scsi_dev_t *)dev->drv_priv : NULL;
    return scsi ? virtio_scsi_flush(&scsi->block) : -1;
}

/* Snapshot of the completion accounting.  irq_count is read with the same
 * relaxed atomic the handler bumps it with: the handler never takes
 * dev->lock, so taking it here would not order the two against each other
 * anyway.  Every other counter is written under dev->lock, so it is read the
 * same way -- the counters are independent, and a reader that wants a
 * consistent pair should compare deltas across two calls, not fields within
 * one. */
static int virtio_scsi_class_ioctl(device_t *dev, unsigned long req, void *arg) {
    virtio_scsi_dev_t *scsi = dev ? (virtio_scsi_dev_t *)dev->drv_priv : NULL;
    if (!scsi)
        return -ENODEV;
    if (req != BLK_IOCTL_GET_STATS || !arg)
        return -ENOTTY;
    a20_blk_stats_t st;
    st.version = A20_BLK_STATS_VERSION;
    st.irq_count = __atomic_load_n(&scsi->irq_count, __ATOMIC_RELAXED);
    mutex_lock(&scsi->lock);
    st.commands = scsi->commands;
    st.flushes = scsi->flushes;
    st.timeouts = scsi->timeouts;
    st.irq_completions = scsi->irq_completions;
    st.spin_completions = scsi->spin_completions;
    st.irq_mode = (uint64_t)scsi->irq_mode;
    st.irq_line = (uint64_t)(int64_t)scsi->vt.irq;
    mutex_unlock(&scsi->lock);
    return copy_to_user(arg, &st, sizeof(st)) < 0 ? -EFAULT : 0;
}

static uint32_t virtio_scsi_class_sector_size(device_t *dev) { (void)dev; return 512; }
static const block_dev_ops_t virtio_scsi_ops = {
    .read = virtio_scsi_class_read, .write = virtio_scsi_class_write,
    .flush = virtio_scsi_class_flush, .ioctl = virtio_scsi_class_ioctl,
    .capacity = virtio_scsi_class_capacity, .sector_size = virtio_scsi_class_sector_size,
};

static const device_id_t virtio_scsi_ids[] = {
    /* modern (virtio-1.0 only) 1af4:1048 = 0x1040 + VIRTIO_ID_SCSI, and the
     * transitional 1af4:1004 QEMU presents when it also offers the legacy
     * interface.  The transitional PCI device IDs are fixed legacy values,
     * not 0x1000 + type: scsi is 1004 even though VIRTIO_ID_SCSI is 8, which
     * is what QEMU puts in the subsystem device ID. */
    { .vendor = 0x1AF4, .device = 0x1048,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = 0x1AF4, .device = 0x1004, .subvendor = VENDOR_ANY, .subdevice = 8 },
    { 0 },
};

static driver_t virtio_scsi_driver = {
    .name = "virtio-scsi", .id_table = virtio_scsi_ids, .bus = &pci_bus,
    .probe = virtio_scsi_probe, .remove = virtio_scsi_remove,
    .class_ops = &virtio_scsi_ops,
    .class_type = DEV_CLASS_BLOCK,
};

DRIVER_REGISTER(virtio_scsi_driver);

block_dev_t *virtio_scsi_get_dev(int index) {
    if (index < 0 || index >= g_scsi_count || !g_scsi[index].ready)
        return NULL;
    return &g_scsi[index].block;
}

int virtio_scsi_ready(int index) {
    return virtio_scsi_get_dev(index) != NULL;
}
