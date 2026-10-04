#include "pci.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/block/virtio_blk.h"
#include "platform.h"
#include "core/stdio.h"
#include "core/string.h"

/* ECAM is a window of dword-granular accesses, not of individual config bytes:
 * the address itself carries the coordinates as
 * bus[31:20] | device[19:15] | function[14:12] | register[11:2], and the low two
 * bits are not address bits at all.  reg is therefore masked down to its dword
 * and a byte or halfword offset has to be extracted afterwards -- the
 * ecam_read16/ecam_read8 helpers below are what makes that shift explicit.
 * The window base is PCIE_ECAM_BASE with a PCIE_ECAM_SIZE of 0x8000000, i.e.
 * 2^27, which is exactly the 8-bit bus field above times 4 MiB per bus: 128
 * buses, matching PCIE_BUS_END = 127.  The 5-bit device field is PCI_MAX_DEV. */
static uint32_t ecam_read(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg) {
    uintptr_t addr = PCIE_ECAM_BASE
        | ((uint32_t)bus << 20)
        | ((uint32_t)dev << 15)
        | ((uint32_t)func << 12)
        | (reg & 0xFFC);
    return *(volatile uint32_t *)addr;
}

/* reg & ~3 rounds down to the containing dword, and (reg & 2) * 8 is the shift
 * for the second halfword: a halfword at offset 0x06 therefore costs a read of
 * 0x04, not a read at address base+6. */
static uint16_t ecam_read16(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg) {
    uint32_t word = ecam_read(bus, dev, func, reg & ~3);
    return (uint16_t)(word >> ((reg & 2) * 8));
}

/* (reg & 3) * 8 is the byte shift within the dword; reading a byte at an odd
 * config offset is still a 32-bit access at the rounded-down address. */
static uint8_t ecam_read8(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg) {
    uint32_t word = ecam_read(bus, dev, func, reg & ~3);
    return (uint8_t)(word >> ((reg & 3) * 8));
}

static void ecam_write(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg, uint32_t val) {
    uintptr_t addr = PCIE_ECAM_BASE
        | ((uint32_t)bus << 20)
        | ((uint32_t)dev << 15)
        | ((uint32_t)func << 12)
        | (reg & 0xFFC);
    *(volatile uint32_t *)addr = val;
}

static int pci_inited = 0;
/* Eight slots for PCI_MAX_DEV devices on the bus.  pci_init() is the only
 * writer and the scan walks PCI_MAX_DEV entries, so a bus with more virtio
 * devices than slots overflows this array -- the count has to be clamped before
 * the loop is lengthened. */
static pci_virtio_dev_t pci_devs[8];
static int pci_ndevs = 0;

static uint64_t pci_mmio_alloc = PCIE_MMIO_BASE;

/* BARs start at 0x10 and are one 32-bit word apart, six of them in a Type 0
 * header.  Bus is hard-wired to 0 and function to 0 throughout this file: the
 * scan below only ever looks at device 0 of bus 0, so a device behind a bridge
 * or on another function is invisible to it by construction. */
static uint32_t read_bar(int dev, int bar) {
    return ecam_read(0, dev, 0, PCI_BAR0 + bar * 4);
}

static uintptr_t alloc_bar_mem(uint8_t dev, int bar) {
    /* BAR sizing: the only way to learn a BAR's size and type is to write all
     * ones to it, read back the bits the device drives (the low address bits
     * float to 0), and put the original value back.  The value has to be
     * restored even when sizing fails below, or the device is left with a
     * garbage BAR that nothing ever re-enables. */
    uint32_t old = read_bar(dev, bar);
    ecam_write(0, dev, 0, PCI_BAR0 + bar * 4, 0xFFFFFFFF);
    uint32_t size_word = ecam_read(0, dev, 0, PCI_BAR0 + bar * 4);
    ecam_write(0, dev, 0, PCI_BAR0 + bar * 4, old);

    /* A BAR that reads back as all ones is unimplemented, and one that reads
     * back as all zeros is a hardwired zero -- an empty BAR.  Both are skipped
     * rather than allocated, which is why the count of allocated windows is not
     * the count of BARs. */
    if (size_word == 0 || size_word == 0xFFFFFFFF)
        return 0;

    /* Bit 0 is the I/O-space bit: 1 means a port window, 0 a memory window.
     * The mask has to clear the low type bits, which are read-only and stay set
     * in the size word -- leaving bit 0 in the mask would make every I/O BAR
     * come out one bit too small. */
    int is_io = size_word & 1;
    uint32_t mask = is_io ? ~0x3UL : ~0xFUL;
    /* The write-back pattern is ~size, so the two's complement of the masked
     * value is the size; this is a power of two, which is why the alignment
     * below can use a plain mask instead of a division. */
    uint32_t size = ~(size_word & mask) + 1;

    /* Bump allocation inside the PCIE_MMIO window, aligned up to the BAR's own
     * size because the address written into the BAR must satisfy every bit the
     * sizing step just reported as 0.  The window ends at PCIE_MMIO_BASE +
     * PCIE_MMIO_SIZE and nothing here checks against it: BARs are programmed in
     * scan order, so a device that does not fit lands outside the window and
     * its accesses fault rather than silently overlapping another device. */
    uintptr_t addr = (pci_mmio_alloc + size - 1) & ~(size - 1);
    pci_mmio_alloc = addr + size;

    /* Only the low half is written: this file never follows the 64-bit pair, so
     * a 64-bit BAR is sized as its low half and its upper half stays whatever
     * the reset value was.  That is consistent for every address this file
     * hands out (all below 4 GiB, PCIE_MMIO_BASE = 0x40000000), so the missing
     * high half is zero in practice -- but a 32-bit BAR that reads as 64-bit
     * would be programmed against a window the driver then has to find. */
    ecam_write(0, dev, 0, PCI_BAR0 + bar * 4, (uint32_t)addr);
    return addr;
}

static void alloc_device_bars(int dev) {
        /* PCI_HEADER_TYPE[2:0] is the layout: 0 is a Type 0 endpoint header
         * with six BARs, 1 is a Type 1 bridge header with only two (and 0x2 is
         * the CardBus header, which this test lumps in with Type 0).  Bit 7 of
         * the same byte is the multifunction flag and is masked off here, so a
         * multifunction device's function 0 does not make max_bar collapse. */
        uint8_t ht = ecam_read8(0, dev, 0, PCI_HEADER_TYPE);
        int hdr_type = ht & 0x7F;
        int max_bar = (hdr_type == 1) ? 2 : 6; // Type 1 is a bridge (2 BARs), Type 0 is a plain device (6 BARs)

    for (int i = 0; i < max_bar; i++) {
        uint32_t bar_val = read_bar(dev, i);
        if (bar_val == 0)
            continue;

        /* bit 0 is the I/O-space bit and bits [2:1] the type field: 00 is a
         * 32-bit memory BAR, 10 is a 64-bit one that is continued in the very
         * next BAR (the low half carries the address, the high half the upper
         * 32 bits). */
        int is_io = bar_val & 1;
        int is_64 = (!is_io) && (((bar_val >> 1) & 0x3) == 2);

        if (!is_io) {
            alloc_bar_mem(dev, i);
            if (is_64 && i + 1 < max_bar) {
                /* Skip the continuation half: write 0 to it rather than sizing
                 * and allocating it as if it were a window of its own, which
                 * would consume a second pci_mmio_alloc region that the device
                 * can never use.  The i++ then keeps the loop from treating the
                 * high half as a fresh BAR on the next pass. */
                ecam_write(0, dev, 0, PCI_BAR0 + (i + 1) * 4, 0);
                i++;
            }
        }
    }
}

/* PCI_COMMAND[1] MEMORY enables the device to *decode* its memory BARs, and
 * [2] BUS_MASTER lets it run DMA onto the system -- a virtio device whose
 * BARs are programmed but with BUS_MASTER clear reads its queues and never
 * writes a used ring.  Bit 0 (I/O space) is deliberately not set: nothing here
 * allocates a port window.  Both are OR-ed in rather than assigned so a
 * previously enabled bit is preserved. */
static void enable_device(int dev) {
    uint32_t cmd = ecam_read(0, dev, 0, PCI_COMMAND);
    cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
    ecam_write(0, dev, 0, PCI_COMMAND, cmd);
}

static int find_virtio_caps(int dev, pci_virtio_dev_t *vd) {
    /* PCI_STATUS[4] is the capability-list bit; with it clear the device has no
     * capabilities at all and 0x34 is not even a valid pointer, which is the only
     * safe reason to bail out before following one. */
    uint16_t status = ecam_read16(0, dev, 0, PCI_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST))
        return -1;

    /* 0x34 is a *byte* register whose low two bits are reserved: every
     * capability is dword-aligned, so masking the pointer to 0xFC here is what
     * makes the successive reads land on whole dwords instead of off by one to
     * three bytes. */
    uint8_t ptr = ecam_read8(0, dev, 0, PCI_CAPABILITIES_PTR) & 0xFC;
    int found = 0;

    /* ptr == 0 ends the list, and ptr < 0xFF additionally stops a malformed or
     * cyclic list from running the loop off the end of the config space. */
    while (ptr && ptr < 0xFF) {
        /* One dword holds the whole header: cap_id in byte 0, the offset of
         * the next capability in byte 1, the length in bytes in byte 2 (unused
         * here -- the fixed virtio layout is walked by +4/+8/+12/+16) and the
         * cfg_type in byte 3.  This is where the little-endian byte order of
         * the config space shows up: the offsets are byte positions, not bit
         * fields. */
        uint32_t cap0 = ecam_read(0, dev, 0, ptr);
        uint8_t cap_id = cap0 & 0xFF;
        uint8_t next = (cap0 >> 8) & 0xFF;

        if (cap_id != PCI_CAP_ID_VNDR) {
            ptr = next & 0xFC;
            continue;
        }

        /* All four virtio capabilities share the same first three fields: +4 is
         * the BAR number the structure lives in, +8 the offset of the structure
         * within that BAR, +12 its length in bytes.  So the register addresses
         * built below are BAR base + cap_offset, and the length is what bounds a
         * read of the device configuration -- the check that keeps a CONFIG read
         * from walking into the next structure. */
        uint8_t cfg_type = (cap0 >> 24) & 0xFF;
        uint32_t bar_word = ecam_read(0, dev, 0, ptr + 4);
        uint8_t bar_idx = bar_word & 0xFF;

        uint32_t cap_offset = ecam_read(0, dev, 0, ptr + 8);
        // the length field is at ptr + 12

        /* The BAR number here indexes the header, not the allocated window, so
         * it is the *programmed* value that is read: clearing the low four bits
         * turns a 64-bit BAR's low half into the base of the structure.  If
         * alloc_device_bars() never reached this BAR the value is still the
         * reset value and every base below points nowhere. */
        uint32_t bar_val = read_bar(dev, bar_idx);
        uintptr_t bar_base = bar_val & ~0xFUL;

        switch (cfg_type) {
        case VIRTIO_PCI_CAP_COMMON_CFG:
            vd->common_base = bar_base + cap_offset;
            found |= 1;
            break;
        case VIRTIO_PCI_CAP_NOTIFY_CFG:
            vd->notify_base = bar_base + cap_offset;
            /* Byte +16 of the notify capability is the notify_off_multiplier, a
             * 32-bit factor applied to queue_notify_off; see the
             * VIRTIO_MMIO_QUEUE_NOTIFY write below for the address it produces.
             * It is read once here, not per queue. */
            vd->notify_off_multiplier = ecam_read(0, dev, 0, ptr + 16);
            found |= 2;
            break;
        case VIRTIO_PCI_CAP_ISR_CFG: 
            vd->isr_base = bar_base + cap_offset;
            found |= 8;
            break;
        case VIRTIO_PCI_CAP_DEVICE_CFG:
            vd->config_base = bar_base + cap_offset;
            found |= 4;
            break;
        }

        ptr = next & 0xFC;
    }

    /* Mask 7 keeps bits for COMMON (1), NOTIFY (2) and DEVICE_CFG (4) only, so
     * those three are required and the ISR capability (8) is not: a device
     * without it is still accepted here, which is why the read path below
     * guards every ISR access on isr_base instead of assuming one exists.  Note
     * that a bare 0 from find_virtio_caps() means "usable" and -1 means "not",
     * so callers must compare against 0 explicitly. */
    return (found & 7) == 7 ? 0 : -1;
}

void pci_init(void) {
    if (pci_inited) return;
    pci_inited = 1;

    printf("[PCI] Scanning bus 0...\n");

    for (int dev = 0; dev < PCI_MAX_DEV; dev++) {
        /* Offset 0 is vendor ID in the low halfword and device ID in the high
         * one, little-endian, so the split is a shift rather than a field.  All
         * ones and all zeros are both "no device here" readings: the first is an
         * open bus, the second a powered-down slot. */
        uint32_t id = ecam_read(0, dev, 0, 0);
        uint16_t vendor = id & 0xFFFF;
        uint16_t device = (id >> 16) & 0xFFFF;

        if (vendor == 0xFFFF || vendor == 0)
            continue;

        if (vendor != PCI_VENDOR_ID_REDHAT)
            continue;

        /* A transitional virtio PCI device encodes its type in the device ID
         * as 0x1040 + type (1 net, 2 block).  When that test fails the type has
         * to come from the subsystem device ID at 0x2E, which is where a
         * non-transitional modern device puts it -- 0x2C is the subsystem
         * *vendor* halfword, and reading the dword at 0x2C then taking the high
         * halfword is how the low one is skipped. */
        int virtio_type = 0;
        if (device >= 0x1040) {
            virtio_type = device - 0x1040;
        } else {
            uint32_t sub = ecam_read(0, dev, 0, 0x2C);
            uint16_t sub_dev = (sub >> 16) & 0xFFFF;
            virtio_type = sub_dev;
        }

        /* Only net (1) and block (2) are claimed; every other virtio type on
         * the bus is skipped without its BARs being touched, so this list is
         * not a picture of what the controller can see, only of what is bound. */
        if (virtio_type != 1 && virtio_type != 2)
            continue;

        /* BARs first, enable second: MEMORY decoding must not be on while the
         * BARs still hold the sizing pattern or a zero, or the device decodes
         * accesses at an address that is not its window. */
        alloc_device_bars(dev);
        enable_device(dev);

        pci_virtio_dev_t *vd = &pci_devs[pci_ndevs];
        memset(vd, 0, sizeof(*vd));
        vd->dev_num = dev;
        vd->device_type = virtio_type;

        /* The device is kept in the array but left invalid if the capabilities
         * do not add up, and pci_ndevs is not advanced -- so a rejected device
         * does not consume an index that arch_virtio_probe_type() later hands
         * out as a probe count.  The BARs it was already given stay programmed;
         * only MEMORY decoding is left enabled, which is why an invalid entry can
         * never be reached. */
        if (find_virtio_caps(dev, vd) != 0)
            continue;

        vd->valid = 1;
        pci_ndevs++;
        printf("[PCI] Found virtio-%s at 00:%02x.0\n",
               virtio_type == 1 ? "net" : "blk", dev);
    }

    printf("[PCI] Found %d virtio device(s)\n", pci_ndevs);
}

/* ---- PCI modern transport ---- */

static uint32_t pci_common_read32(uintptr_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static void pci_common_write32(uintptr_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}

static uint8_t pci_common_read8(uintptr_t base, uint32_t off) {
    return *(volatile uint8_t *)(base + off);
}

static void pci_common_write8(uintptr_t base, uint32_t off, uint8_t val) {
    *(volatile uint8_t *)(base + off) = val;
}

static uint16_t pci_common_read16(uintptr_t base, uint32_t off) {
    return *(volatile uint16_t *)(base + off);
}

static void pci_common_write16(uintptr_t base, uint32_t off, uint16_t val) {
    *(volatile uint16_t *)(base + off) = val;
}

/* PCI common cfg register offsets */
/* The layout below is the virtio PCI common configuration structure.  Each half
 * of a 64-bit feature set is viewed through a 32-bit window chosen by its own
 * SEL register, so anything above bit 31 needs a second round with SEL = 1;
 * DEVICE_FEATURES is device-to-driver and DRIVER_FEATURES is the mirror image,
 * driver-to-device, and in both cases bit 0 of the window is bit 0 of that
 * half.  Below the feature block every register is 16 bits wide, which is why
 * the file has byte, halfword and dword accessors for the same base rather than
 * one generic accessor -- see the PCOMMON_STATUS and PCOMMON_QUEUE_SEL cases. */
#define PCOMMON_DEV_FEAT_SEL   0x00
#define PCOMMON_DEV_FEAT       0x04
#define PCOMMON_DRV_FEAT_SEL   0x08
#define PCOMMON_DRV_FEAT       0x0C
#define PCOMMON_STATUS         0x14
#define PCOMMON_QUEUE_SEL      0x16
#define PCOMMON_QUEUE_SIZE     0x18
#define PCOMMON_QUEUE_ENABLE   0x1C
/* The three 64-bit queue addresses are consecutive 32-bit halves: the
 * descriptor table at 0x20/0x24, the available ring (driver area) at 0x28/0x2C
 * and the used ring (device area) at 0x30/0x34.  The low half is written first
 * and the high half second, so the address is never half-visible to the
 * device.  Their contents and their publication order are the driver's
 * contract in kernel/include/drivers/dual/virtq.h, not this file's. */
#define PCOMMON_QUEUE_DESC_LO  0x20
#define PCOMMON_QUEUE_DESC_HI  0x24
#define PCOMMON_QUEUE_DRV_LO   0x28
#define PCOMMON_QUEUE_DRV_HI   0x2C
#define PCOMMON_QUEUE_DEV_LO   0x30
#define PCOMMON_QUEUE_DEV_HI   0x34
#define PCOMMON_QUEUE_NOTIFY_OFF 0x1E

typedef struct {
    pci_virtio_dev_t *pdev;
} pci_transport_priv_t;

static uint32_t pci_vt_read32(virtio_transport_t *t, uint32_t mmio_off) {
    pci_transport_priv_t *p = (pci_transport_priv_t *)t->priv;
    pci_virtio_dev_t *vd = p->pdev;
    uintptr_t cb = vd->common_base;

    switch (mmio_off) {
    case VIRTIO_MMIO_DEVICE_FEATURES:
        return pci_common_read32(cb, PCOMMON_DEV_FEAT);
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        return pci_common_read32(cb, PCOMMON_DEV_FEAT_SEL);
    case VIRTIO_MMIO_DRIVER_FEATURES:
        return pci_common_read32(cb, PCOMMON_DRV_FEAT);
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        return pci_common_read32(cb, PCOMMON_DRV_FEAT_SEL);
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
        return pci_common_read16(cb, PCOMMON_QUEUE_SIZE);
    /* PCOMMON_STATUS is a single byte at 0x14, and the next two registers in
     * the dword (config_generation at 0x15, queue_select at 0x16) are not part
     * of it -- which is why this and the matching write go through the 8-bit
     * accessors.  A 32-bit access here would overwrite the queue selection as a
     * side effect of setting the status. */
    case VIRTIO_MMIO_STATUS:
        return pci_common_read8(cb, PCOMMON_STATUS);
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
        return pci_common_read32(cb, PCOMMON_QUEUE_DESC_LO);
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        return pci_common_read32(cb, PCOMMON_QUEUE_DESC_HI);
    case VIRTIO_MMIO_QUEUE_DRIVER_LOW:
        return pci_common_read32(cb, PCOMMON_QUEUE_DRV_LO);
    case VIRTIO_MMIO_QUEUE_DRIVER_HIGH:
        return pci_common_read32(cb, PCOMMON_QUEUE_DRV_HI);
    case VIRTIO_MMIO_QUEUE_DEVICE_LOW:
        return pci_common_read32(cb, PCOMMON_QUEUE_DEV_LO);
    case VIRTIO_MMIO_QUEUE_DEVICE_HIGH:
        return pci_common_read32(cb, PCOMMON_QUEUE_DEV_HI);
    /* The ISR is a single status byte and read-to-clear: reading it is what
     * acknowledges the interrupt, so this read has a side effect and cannot be
     * repeated "just to look".  The modern transport has no separate ack
     * register, and the whole register is one byte, so the 32-bit transport
     * result carries it in the low byte.  A zero is returned when the device
     * exposed no ISR capability at all (see find_virtio_caps), which is
     * indistinguishable from "no interrupt pending" -- correct for a driver
     * that polls, but it means a missing ISR capability can never be reported
     * as an error from here. */
    case VIRTIO_MMIO_INTERRUPT_STATUS:
        if (vd->isr_base)
            return *(volatile uint8_t *)(vd->isr_base); // the ISR register is 8 bits wide
        return 0;
    /* Device configuration is BAR-relative, at whatever offset the DEVICE_CFG
     * capability named, and it is plain device-defined data rather than a fixed
     * register block: only the first two dwords are reachable through this
     * transport, so any feature that needs more configuration space than that
     * cannot be negotiated here.  The virtio block config is 60 bytes, so a
     * device that needs all of it is out of reach. */
    case VIRTIO_MMIO_CONFIG:
        return *(volatile uint32_t *)(vd->config_base + 0);
    case VIRTIO_MMIO_CONFIG + 4:
        return *(volatile uint32_t *)(vd->config_base + 4);
    /* The two queue-setup registers that follow belong to the legacy (v0.9)
     * interface: GUEST_PAGE_SIZE and the page-frame number that together replace
     * the three address registers above.  Returning 0 for them is unreachable
     * in practice because every probe below sets legacy = 0, and a driver that
     * did take the legacy path would see a queue size of 0 and stop there. */
    case VIRTIO_MMIO_GUEST_PAGE_SIZE:
    case VIRTIO_MMIO_QUEUE_PFN:
        return 0;
    default:
        return 0;
    }
}

static void pci_vt_write32(virtio_transport_t *t, uint32_t mmio_off, uint32_t val) {
    pci_transport_priv_t *p = (pci_transport_priv_t *)t->priv;
    pci_virtio_dev_t *vd = p->pdev;
    uintptr_t cb = vd->common_base;

    switch (mmio_off) {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        pci_common_write32(cb, PCOMMON_DEV_FEAT_SEL, val);
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES:
        pci_common_write32(cb, PCOMMON_DRV_FEAT, val);
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        pci_common_write32(cb, PCOMMON_DRV_FEAT_SEL, val);
        break;
    /* Same one-byte field as on the read side: ACK (0x00), DRIVER_OK (0x04) and
     * FEATURES_OK (0x08) are bits of one byte, and 0 is a valid value to write,
     * so the write cannot be skipped on "nothing to set" grounds. */
    case VIRTIO_MMIO_STATUS:
        pci_common_write8(cb, PCOMMON_STATUS, (uint8_t)val);
        break;
    /* queue_select is 16 bits at 0x16 and is what makes every other queue
     * register below refer to one virtqueue, so it is written 16 bits wide: a
     * 32-bit store would run into queue_size at 0x18 and write a size of 0
     * while selecting the queue.  queue_size is 16 bits and must be a power of
     * two; queue_enable at 0x1C is 16 bits and is what actually starts the
     * queue running.  Between them at 0x1A sits queue_msix_vector, which this
     * transport has no use for and so never writes. */
    case VIRTIO_MMIO_QUEUE_SEL:
        pci_common_write16(cb, PCOMMON_QUEUE_SEL, (uint16_t)val);
        break;
    case VIRTIO_MMIO_QUEUE_NUM:
        pci_common_write16(cb, PCOMMON_QUEUE_SIZE, (uint16_t)val);
        break;
    case VIRTIO_MMIO_QUEUE_READY:
        pci_common_write16(cb, PCOMMON_QUEUE_ENABLE, (uint16_t)val);
        break;
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
        pci_common_write32(cb, PCOMMON_QUEUE_DESC_LO, val);
        break;
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        pci_common_write32(cb, PCOMMON_QUEUE_DESC_HI, val);
        break;
    case VIRTIO_MMIO_QUEUE_DRIVER_LOW:
        pci_common_write32(cb, PCOMMON_QUEUE_DRV_LO, val);
        break;
    case VIRTIO_MMIO_QUEUE_DRIVER_HIGH:
        pci_common_write32(cb, PCOMMON_QUEUE_DRV_HI, val);
        break;
    case VIRTIO_MMIO_QUEUE_DEVICE_LOW:
        pci_common_write32(cb, PCOMMON_QUEUE_DEV_LO, val);
        break;
    case VIRTIO_MMIO_QUEUE_DEVICE_HIGH:
        pci_common_write32(cb, PCOMMON_QUEUE_DEV_HI, val);
        break;
    /* A kick is three steps, and the value written here is the virtqueue index
     * itself, not 0 or 1.  Step 1: select the queue (16 bits at 0x16).  Step 2:
     * read that queue's notify offset, 16 bits at 0x1E, which is relative to
     * the notify capability's own BAR offset.  Step 3: the notification address
     * is notify_base + queue_notify_off * notify_off_multiplier, and the store
     * to it is 16 bits wide.  The multiplication is 32-bit arithmetic on a
     * 32-bit multiplier, so a multiplier that does not fit truncates and the
     * write lands inside the BAR instead of raising an interrupt; and writing
     * the queue index without step 1 kicks whichever queue was selected last. */
    case VIRTIO_MMIO_QUEUE_NOTIFY: {
        pci_common_write16(cb, PCOMMON_QUEUE_SEL, (uint16_t)val);
        uint16_t qidx = (uint16_t)val;
        uint16_t notify_off;
        if (qidx < PCI_VIRTIO_NOTIFY_CACHE &&
            (vd->notify_off_cached & (1U << qidx))) {
            notify_off = vd->notify_off_cache[qidx];
        } else {
            notify_off = pci_common_read16(cb, PCOMMON_QUEUE_NOTIFY_OFF);
            if (qidx < PCI_VIRTIO_NOTIFY_CACHE) {
                vd->notify_off_cache[qidx] = notify_off;
                vd->notify_off_cached |= 1U << qidx;
            }
        }
        uintptr_t addr = vd->notify_base + notify_off * vd->notify_off_multiplier;
        *(volatile uint16_t *)addr = (uint16_t)val;
        break;
    }
    /* Same legacy queue-setup pair as on the read side: dropping a write to
     * GUEST_PAGE_SIZE or QUEUE_PFN silently is safe only because no probe below
     * ever sets legacy = 1.  A driver that took the legacy path would watch
     * QUEUE_PFN reads return 0 and stall with no diagnostic pointing here. */
    case VIRTIO_MMIO_GUEST_PAGE_SIZE:
    case VIRTIO_MMIO_QUEUE_PFN:
        break;
    default:
        break;
    }
}

/* One private per bound device, same eight-slot bound as pci_devs above.  The
 * bound is checked in arch_virtio_probe_type() before the array is indexed,
 * unlike the device list, because this one is filled on a later path. */
static pci_transport_priv_t pci_privs[8];
static int pci_npriv;

static int arch_virtio_probe_type(int type, int index, virtio_transport_t *vt) {
    pci_init();

    pci_virtio_dev_t *vd = NULL;
    /* index counts devices *of this type* in enumeration order, not ECAM slot
     * numbers, so a device skipped by pci_init() does not create a gap and a
     * caller probing 0, 1, 2 … walks the valid entries without repeats.  Order
     * is bus 0, device 0 … 31, function 0, which is the pci_init() scan order. */
    int seen = 0;
    for (int i = 0; i < pci_ndevs; i++) {
        if (!pci_devs[i].valid || pci_devs[i].device_type != type)
            continue;
        if (seen++ == index) {
            vd = &pci_devs[i];
            break;
        }
    }
    if (!vd)
        return -1;

    if (pci_npriv >= (int)(sizeof(pci_privs) / sizeof(pci_privs[0])))
        return -1;
    pci_transport_priv_t *priv = &pci_privs[pci_npriv++];
    priv->pdev = vd;

    /* legacy = 0 selects the modern interface, which is what makes the
     * GUEST_PAGE_SIZE/QUEUE_PFN stubs above unreachable.  irq = -1 means this
     * transport is not IRQ-driven: no INTx routing is programmed (PCI_INTERRUPT
     * _LINE is never written) and no MSI-X table is built, so the device is left
     * with interrupts disabled and the driver polls the ISR byte.  That is the
     * deliberate legacy path the LoongArch board docs describe, and it is why
     * the shared shared_irq flag stays 0 -- there is no line to share. */
    vt->read32 = pci_vt_read32;
    vt->write32 = pci_vt_write32;
    vt->priv = priv;
    vt->legacy = 0;
    vt->irq = -1;
    return 0;
}

int arch_virtio_blk_probe(int index, virtio_transport_t *vt) {
    return arch_virtio_probe_type(2, index, vt);
}

int arch_virtio_net_probe(int index, virtio_transport_t *vt) {
    return arch_virtio_probe_type(1, index, vt);
}
