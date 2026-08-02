#include "core/stdio.h"
#include "core/string.h"
#include "core/errno.h"
#include "fs/vfs.h"
#include "fs/mount_setup.h"
#include "fs/block_cache.h"
#include "drivers/block/virtio_blk.h"
#include "drivers/block/virtio_scsi.h"
#include "drivers/block/ahci.h"
#include "drivers/usb/usb_storage.h"

/*
 * Block-device mount strategy, split out of kernel/main.c.
 *
 * Probes all virtio-blk / virtio-scsi / AHCI / USB-storage devices and
 * auto-detects filesystems:
 *   fat32 -> /bin   (our utilities: init, mksh, cmds, ...)
 *   ext4  -> /test  (judge sdcard or local sdcard image)
 *
 * Works regardless of device ordering:
 *   Contest QEMU:  dev0=ext4(sdcard) dev1=fat32(disk.img)
 *   Dev QEMU:      dev0=fat32(disk.img) dev1=ext4(sdcard)
 */

#ifndef BRINGUP
typedef struct {
    block_dev_t block;
    block_dev_t *parent;
    uint64_t first_lba;
    uint64_t sectors;
} partition_block_dev_t;

static int partition_read_sector(block_dev_t *block, uint64_t lba, void *buf,
                                 size_t count) {
    partition_block_dev_t *part = (partition_block_dev_t *)block->priv;
    if (!part || !part->parent || lba > part->sectors ||
        count > part->sectors - lba)
        return -1;
    return part->parent->read_sector(part->parent, part->first_lba + lba, buf, count);
}

static int partition_write_sector(block_dev_t *block, uint64_t lba, const void *buf,
                                  size_t count) {
    partition_block_dev_t *part = (partition_block_dev_t *)block->priv;
    if (!part || !part->parent || lba > part->sectors ||
        count > part->sectors - lba)
        return -1;
    return part->parent->write_sector(part->parent, part->first_lba + lba, buf, count);
}

/* The VBox UEFI image is GPT-partitioned.  Expose its first partition to the
 * existing FAT/ext4 mount code instead of assuming a superfloppy image. */
static block_dev_t *first_gpt_partition(block_dev_t *parent) {
    static partition_block_dev_t partition;
    uint8_t entry[128];
    uint8_t header[512];
    if (!parent || !parent->read_sector || parent->read_sector(parent, 1, header, 1) != 0)
        return NULL;
    if (memcmp(header, "EFI PART", 8) != 0)
        return NULL;

    uint64_t entries_lba = 0;
    for (int i = 0; i < 8; i++)
        entries_lba |= (uint64_t)header[72 + i] << (i * 8);
    uint32_t entry_size = (uint32_t)header[84] | ((uint32_t)header[85] << 8) |
                          ((uint32_t)header[86] << 16) | ((uint32_t)header[87] << 24);
    if (!entries_lba || entry_size < sizeof(entry) || entry_size > 512 ||
        parent->read_sector(parent, entries_lba, entry, 1) != 0)
        return NULL;

    uint64_t first = 0, last = 0;
    for (int i = 0; i < 8; i++) {
        first |= (uint64_t)entry[32 + i] << (i * 8);
        last |= (uint64_t)entry[40 + i] << (i * 8);
    }
    if (!first || last < first || last >= parent->capacity)
        return NULL;
    partition.parent = parent;
    partition.first_lba = first;
    partition.sectors = last - first + 1;
    partition.block.read_sector = partition_read_sector;
    partition.block.write_sector = partition_write_sector;
    partition.block.capacity = partition.sectors;
    partition.block.sector_size = parent->sector_size;
    partition.block.priv = &partition;
    return &partition.block;
}

static int try_mount(block_dev_t *dev, const char *mnt, const char *fstype) {
    if (!dev) return -1;
    bcache_t *bc = bcache_create(dev);
    if (!bc) return -1;
    int mkret = vfs_mkdir(mnt, 0755);
    if (mkret < 0 && mkret != -EEXIST) {
        bcache_destroy(bc);
        return mkret;
    }
    int r = vfs_mount_bc(mnt, fstype, bc);
    if (r == 0) {
        printf("[INIT] Block device -> %s (%s)\n", mnt, fstype);
    } else {
        bcache_destroy(bc);
    }
    return r;
}

static void mount_final_root_pseudo_filesystems(void) {
    struct {
        const char *path;
        const char *dev;
        const char *fstype;
    } mounts[] = {
        { "/test/dev",     "none",  "devtmpfs" },
        { "/test/dev/shm", "none",  "tmpfs" },
        { "/test/proc",    "proc",  "proc" },
        { "/test/sys",     "sysfs", "sysfs" },
    };

    /*
     * These directories already exist in the published Debian images.  The
     * mkdir calls also make the setup harmless for smaller local ext4 images.
     */
    vfs_mkdir("/test/dev", 0755);
    vfs_mkdir("/test/dev/shm", 01777);
    vfs_mkdir("/test/proc", 0755);
    vfs_mkdir("/test/sys", 0755);

    for (size_t i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++) {
        int r = vfs_mount(mounts[i].dev, mounts[i].path,
                          mounts[i].fstype, 0, NULL);
        if (r < 0)
            printf("[INIT] WARNING: mount %s at %s failed: %d\n",
                   mounts[i].fstype, mounts[i].path, r);
    }
}

void mount_block_devices(void) {
    int bin_ok = 0, test_ok = 0;

    for (int i = 0; i < 8; i++) {
        block_dev_t *blk = virtio_blk_get_dev(i);
        if (!blk)
            break;
        if (!bin_ok && try_mount(blk, "/bin", "fat32") == 0) {
            bin_ok = 1;
            continue;
        }
        if (!test_ok && try_mount(blk, "/test", "ext4") == 0) {
            test_ok = 1;
            continue;
        }
    }

    /* VirtualBox ARM exposes its boot disk through a VirtIO-SCSI controller,
     * not a VirtIO block function. The controller driver is bound during PCI
     * enumeration, so mount any discovered LUNs alongside virtio-blk disks. */
    for (int i = 0; i < 4; i++) {
        block_dev_t *scsi = virtio_scsi_get_dev(i);
        if (!scsi)
            continue;
        block_dev_t *fsdev = first_gpt_partition(scsi);
        if (!fsdev)
            fsdev = scsi;
        if (!bin_ok && try_mount(fsdev, "/bin", "fat32") == 0) {
            bin_ok = 1;
            continue;
        }
        if (!test_ok && try_mount(fsdev, "/test", "ext4") == 0)
            test_ok = 1;
    }

    block_dev_t *ahci = NULL;
#ifdef CONFIG_X86_64
    ahci = ahci_get_dev(0);
#endif
    if (ahci) {
        if (!bin_ok)
            bin_ok = try_mount(ahci, "/bin", "fat32") == 0;
        if (!test_ok)
            test_ok = try_mount(ahci, "/test", "ext4") == 0;
    }

    /* USB mass-storage disks (U-disk / USB HDD).  These enumerate after the
     * PCI/virtio buses and are scanned by usb_core_scan() from the scheduler
     * context, so they are mounted last and only when a core disk is absent. */
    for (int i = 0; i < 4; i++) {
        block_dev_t *usb = usb_storage_get_dev(i);
        if (!usb)
            continue;
        block_dev_t *fsdev = first_gpt_partition(usb);
        if (!fsdev)
            fsdev = usb;
        if (!bin_ok && try_mount(fsdev, "/bin", "fat32") == 0) {
            bin_ok = 1;
            continue;
        }
        if (!test_ok && try_mount(fsdev, "/test", "ext4") == 0)
            test_ok = 1;
    }

    if (!bin_ok)  printf("[INIT] WARNING: no FAT32 device for /bin\n");
    if (!test_ok) {
        printf("[INIT] no ext4 device for /test (ok without sdcard)\n");
    } else {
        mount_final_root_pseudo_filesystems();
    }
}
#endif /* BRINGUP */
