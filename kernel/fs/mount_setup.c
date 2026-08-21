#include "core/stdio.h"
#include "core/string.h"
#include "core/errno.h"
#include "fs/vfs.h"
#include "fs/mount_setup.h"
#include "fs/block_cache.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"

/*
 * Block-device mount strategy, split out of kernel/main.c.
 *
 * Probes all virtio-blk / virtio-scsi / AHCI / USB-storage devices and
 * auto-detects filesystems.  Normal builds retain the legacy
 * layout (FAT32 at /bin, EXT4 at /extra).  External-root builds retain the
 * bootstrap ramfs as /, mount A20OS utilities at /a20, and mount the
 * attached EXT4 image at /mnt; init then runs the shell from /a20, so a
 * failed or unfamiliar system image never removes the bootstrap
 * environment and its diagnostics.
 *
 * Works regardless of device ordering:
 *   Alt QEMU:  dev0=ext4(sdcard) dev1=fat32(disk.img)
 *   Dev QEMU:      dev0=fat32(disk.img) dev1=ext4(sdcard)
 */

#if !defined(BRINGUP) || defined(CONFIG_STORAGE_READ_ONLY)
typedef struct {
    block_dev_t block;
    block_dev_t *parent;
    uint64_t first_lba;
    uint64_t sectors;
    int read_only;
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
    if (part->read_only)
        return -EROFS;
    return part->parent->write_sector(part->parent, part->first_lba + lba, buf, count);
}

typedef struct class_block_dev {
    block_dev_t block;
    device_t *dev;
    const block_dev_ops_t *ops;
} class_block_dev_t;

static int class_block_read_sector(block_dev_t *block, uint64_t lba, void *buf,
                                   size_t count)
{
    class_block_dev_t *class_block = (class_block_dev_t *)block->priv;
    return class_block && class_block->ops && class_block->ops->read ?
        class_block->ops->read(class_block->dev, lba, buf, count) : -1;
}

static int class_block_write_sector(block_dev_t *block, uint64_t lba, const void *buf,
                                    size_t count)
{
    class_block_dev_t *class_block = (class_block_dev_t *)block->priv;
    return class_block && class_block->ops && class_block->ops->write ?
        class_block->ops->write(class_block->dev, lba, buf, count) : -1;
}

block_dev_t *mount_setup_block_device(int index)
{
    static class_block_dev_t class_blocks[16];
    device_t *dev = device_find_by_class(DEV_CLASS_BLOCK, index);
    if (!dev || !dev->drv || !dev->drv->class_ops || index < 0 || index >= 16)
        return NULL;
    const block_dev_ops_t *ops = (const block_dev_ops_t *)dev->drv->class_ops;
    if (!ops->read || !ops->write || !ops->capacity || !ops->sector_size)
        return NULL;
    class_block_dev_t *class_block = &class_blocks[index];
    class_block->dev = dev;
    class_block->ops = ops;
    class_block->block.read_sector = class_block_read_sector;
    class_block->block.write_sector = class_block_write_sector;
    class_block->block.capacity = ops->capacity(dev);
    class_block->block.sector_size = ops->sector_size(dev);
    class_block->block.priv = class_block;
    return &class_block->block;
}

/* Expose individual GPT partitions to the filesystem probe.  VF2's boot
 * image has raw SPL/FIT partitions followed by the user FAT32 partition, so
 * looking only at GPT entry zero can never find /bin. */
static block_dev_t *gpt_partition(block_dev_t *parent, int parent_index,
                                  int ordinal) {
    static partition_block_dev_t partitions[16][16];
    uint8_t entry[128];
    uint8_t entry_sector[512];
    uint8_t header[512];
    if (!parent || parent_index < 0 || parent_index >= 16 || ordinal < 0 ||
        ordinal >= 16 || !parent->read_sector ||
        parent->read_sector(parent, 1, header, 1) != 0)
        return NULL;
    if (memcmp(header, "EFI PART", 8) != 0)
        return NULL;

    uint64_t entries_lba = 0;
    for (int i = 0; i < 8; i++)
        entries_lba |= (uint64_t)header[72 + i] << (i * 8);
    uint32_t entry_count = (uint32_t)header[80] | ((uint32_t)header[81] << 8) |
                           ((uint32_t)header[82] << 16) | ((uint32_t)header[83] << 24);
    uint32_t entry_size = (uint32_t)header[84] | ((uint32_t)header[85] << 8) |
                          ((uint32_t)header[86] << 16) | ((uint32_t)header[87] << 24);
    if (!entries_lba || !entry_count || ordinal >= (int)entry_count ||
        entry_size < sizeof(entry) || entry_size > 512 ||
        parent->read_sector(parent, entries_lba +
                            ((uint64_t)ordinal * entry_size) / 512,
                            entry_sector, 1) != 0)
        return NULL;
    uint32_t entry_offset = ((uint32_t)ordinal * entry_size) % 512;
    if (entry_offset + sizeof(entry) > sizeof(entry_sector))
        return NULL;
    memcpy(entry, entry_sector + entry_offset, sizeof(entry));

    uint64_t first = 0, last = 0;
    for (int i = 0; i < 8; i++) {
        first |= (uint64_t)entry[32 + i] << (i * 8);
        last |= (uint64_t)entry[40 + i] << (i * 8);
    }
    if (!first || last < first || last >= parent->capacity)
        return NULL;
    partition_block_dev_t *partition = &partitions[parent_index][ordinal];
    partition->parent = parent;
    partition->first_lba = first;
    partition->sectors = last - first + 1;
    partition->read_only = 0;
    partition->block.read_sector = partition_read_sector;
    partition->block.write_sector = partition_write_sector;
    partition->block.capacity = partition->sectors;
    partition->block.sector_size = parent->sector_size;
    partition->block.priv = partition;
    return &partition->block;
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static block_dev_t *first_mbr_linux_partition(block_dev_t *parent,
                                               uint64_t *first_lba,
                                               uint64_t *sectors) {
    static partition_block_dev_t partition;
    uint8_t mbr[512];
    if (!parent || !parent->read_sector || parent->sector_size != 512 ||
        parent->read_sector(parent, 0, mbr, 1) != 0) {
        printf("[STORAGE-RO] failed to read MBR sector\n");
        return NULL;
    }
    if (mbr[510] != 0x55 || mbr[511] != 0xaa) {
        printf("[STORAGE-RO] MBR signature missing\n");
        return NULL;
    }

    printf("[STORAGE-RO] MBR signature 55aa verified\n");
    for (unsigned i = 0; i < 4; i++) {
        const uint8_t *entry = mbr + 446 + i * 16;
        uint8_t type = entry[4];
        uint64_t first = read_le32(entry + 8);
        uint64_t count = read_le32(entry + 12);
        if (!type || !count)
            continue;
        printf("[STORAGE-RO] MBR partition %u type=%x start=%lu sectors=%lu\n",
               i + 1, type, (unsigned long)first, (unsigned long)count);
        if (type != 0x83)
            continue;
        if (!first || first >= parent->capacity ||
            count > parent->capacity - first) {
            printf("[STORAGE-RO] Linux partition %u exceeds disk bounds\n",
                   i + 1);
            return NULL;
        }

        memset(&partition, 0, sizeof(partition));
        partition.parent = parent;
        partition.first_lba = first;
        partition.sectors = count;
        partition.read_only = 1;
        partition.block.read_sector = partition_read_sector;
        partition.block.write_sector = partition_write_sector;
        partition.block.capacity = count;
        partition.block.sector_size = parent->sector_size;
        partition.block.priv = &partition;
        if (first_lba)
            *first_lba = first;
        if (sectors)
            *sectors = count;
        return &partition.block;
    }
    printf("[STORAGE-RO] no primary Linux partition found\n");
    return NULL;
}

int try_mount(block_dev_t *dev, const char *mnt, const char *fstype) {
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

static int try_mount_read_only(block_dev_t *dev, const char *mnt,
                               const char *fstype) {
    if (!dev)
        return -ENODEV;
    bcache_t *bc = bcache_create(dev);
    if (!bc)
        return -ENOMEM;
    int mkret = vfs_mkdir(mnt, 0755);
    if (mkret < 0 && mkret != -EEXIST) {
        bcache_destroy(bc);
        return mkret;
    }
    int ret = vfs_mount_bc_flags(mnt, fstype, bc, VFS_MOUNT_RDONLY);
    if (ret < 0) {
        bcache_destroy(bc);
        return ret;
    }
    printf("[STORAGE-RO] mounted %s read-only at %s\n", fstype, mnt);
    return 0;
}

void mount_read_only_storage(void) {
    printf("[STORAGE-RO] scanning block devices; writes are disabled\n");
    for (int i = 0; i < 16; i++) {
        block_dev_t *disk = mount_setup_block_device(i);
        if (!disk)
            continue;
        printf("[STORAGE-RO] disk %d capacity=%lu sectors sector_size=%u\n",
               i, (unsigned long)disk->capacity, disk->sector_size);
        uint64_t first = 0, sectors = 0;
        block_dev_t *part = first_mbr_linux_partition(disk, &first, &sectors);
        if (!part)
            continue;
        printf("[STORAGE-RO] selected Linux partition start=%lu sectors=%lu\n",
               (unsigned long)first, (unsigned long)sectors);
        int ret = try_mount_read_only(part, "/extra", "ext4");
        if (ret == 0)
            return;
        printf("[STORAGE-RO] ext4 mount refused or failed: %d\n", ret);
    }
    printf("[STORAGE-RO] no clean ext4 partition mounted; RAM shell continues\n");
}

static void mount_external_root_pseudo_filesystems(void) {
#ifdef CONFIG_EXTERNAL_ROOT
#define EXTERNAL_ROOT_PATH(path) "/mnt" path
#else
#define EXTERNAL_ROOT_PATH(path) "/extra" path
#endif
    struct {
        const char *path;
        const char *dev;
        const char *fstype;
    } mounts[] = {
        { EXTERNAL_ROOT_PATH("/dev"),     "none",  "devtmpfs" },
        { EXTERNAL_ROOT_PATH("/dev/shm"), "none",  "tmpfs" },
        { EXTERNAL_ROOT_PATH("/proc"),    "proc",  "proc" },
        { EXTERNAL_ROOT_PATH("/sys"),     "sysfs", "sysfs" },
        { EXTERNAL_ROOT_PATH("/run"),     "none",  "tmpfs" },
    };

    /*
     * These directories already exist in the published Debian images.  The
     * mkdir calls also make the setup harmless for smaller local ext4 images.
     */
    vfs_mkdir(EXTERNAL_ROOT_PATH("/dev"), 0755);
    vfs_mkdir(EXTERNAL_ROOT_PATH("/dev/shm"), 01777);
    vfs_mkdir(EXTERNAL_ROOT_PATH("/proc"), 0755);
    vfs_mkdir(EXTERNAL_ROOT_PATH("/sys"), 0755);
    vfs_mkdir(EXTERNAL_ROOT_PATH("/run"), 0755);
    vfs_mkdir(EXTERNAL_ROOT_PATH("/tmp"), 01777);

    for (size_t i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++) {
        int r = vfs_mount(mounts[i].dev, mounts[i].path,
                          mounts[i].fstype, 0, NULL);
        if (r < 0)
            printf("[INIT] WARNING: mount %s at %s failed: %d\n",
                   mounts[i].fstype, mounts[i].path, r);
    }
#undef EXTERNAL_ROOT_PATH
}

void mount_block_devices(void) {
#ifdef CONFIG_EXTERNAL_ROOT
    const char *utilities_path = "/a20";
    const char *system_path = "/mnt";
#else
    const char *utilities_path = "/bin";
    const char *system_path = "/extra";
#endif
    int utilities_ok = 0, system_ok = 0;

    for (int i = 0; i < 16; i++) {
        block_dev_t *blk = mount_setup_block_device(i);
        if (!blk)
            continue;
        if (!utilities_ok && try_mount(blk, utilities_path, "fat32") == 0) {
            utilities_ok = 1;
            continue;
        }
        if (!system_ok && try_mount(blk, system_path, "ext4") == 0) {
            system_ok = 1;
            continue;
        }
        for (int ordinal = 0; ordinal < 16 && (!utilities_ok || !system_ok);
             ordinal++) {
            block_dev_t *partition = gpt_partition(blk, i, ordinal);
            if (!partition)
                continue;
            if (!utilities_ok &&
                try_mount(partition, utilities_path, "fat32") == 0)
                utilities_ok = 1;
            if (!system_ok &&
                try_mount(partition, system_path, "ext4") == 0)
                system_ok = 1;
        }
    }

    if (!utilities_ok)
        printf("[INIT] WARNING: no FAT32 device for %s\n", utilities_path);
    if (!system_ok) {
        printf("[INIT] no ext4 device for %s (ok without sdcard)\n",
               system_path);
    } else {
        mount_external_root_pseudo_filesystems();
    }
}
#else /* BRINGUP without explicit storage experiment */

/* No block devices exist in BRINGUP builds; keep the class lookup linkable
 * for vfs_mount(/dev/vd*) and the swap path. */
block_dev_t *mount_setup_block_device(int index)
{
    (void)index;
    return NULL;
}

void mount_read_only_storage(void)
{
    printf("[INIT] read-only storage experiment is not enabled\n");
}

#endif /* !BRINGUP || CONFIG_STORAGE_READ_ONLY */
