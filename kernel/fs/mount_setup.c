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

/* Explicit mounts may use already-probed class devices even in a RAM-root
 * bringup. Automatic root probing below retains its existing profile gate. */
#define CLASS_BLOCK_HANDLE_BUDGET 256U
/* Borrowed block_dev handles can outlive unplug in BC/UXFS consumers. Keep a
 * bounded stable descriptor for each class index; retired indices stay offline
 * and are never redirected to a new disk. Device/class references are released
 * at unpublish. Reclaiming descriptors requires owned BC/UXFS handle contracts. */
typedef struct class_block_dev {
    block_dev_t block;
    class_device_t *binding;
    int used;
} class_block_dev_t;
static class_block_dev_t class_blocks[CLASS_BLOCK_HANDLE_BUDGET];
static spinlock_t class_blocks_lock = SPINLOCK_INIT;
static class_device_t *class_block_ref(block_dev_t *block) {
    if (!block || !block->priv) return NULL;
    class_block_dev_t *handle = block->priv;
    uint64_t flags = spin_lock_irqsave(&class_blocks_lock);
    class_device_t *binding = handle->binding;
    class_device_get(binding);
    spin_unlock_irqrestore(&class_blocks_lock, flags);
    return binding;
}
static void class_block_offline(class_device_t *binding, void *data) {
    class_block_dev_t *handle = data;
    uint64_t flags = spin_lock_irqsave(&class_blocks_lock);
    int owned = handle->binding == binding;
    if (owned) handle->binding = NULL;
    spin_unlock_irqrestore(&class_blocks_lock, flags);
    if (owned) class_device_put(binding);
}
#define CLASS_BLOCK_OPERATION(name, arguments, expression) \
static int name arguments { \
    class_device_t *binding = class_block_ref(block); \
    const void *table; \
    int rc = class_device_ops_begin(binding, DEV_CLASS_BLOCK, &table); \
    if (!rc) { \
        const block_dev_ops_t *ops = table; \
        rc = (expression); \
        class_device_call_end(binding); \
    } \
    class_device_put(binding); \
    return rc; \
}
CLASS_BLOCK_OPERATION(class_block_read_sector,
    (block_dev_t *block, uint64_t lba, void *buf, size_t count),
    ops->read ? ops->read(binding->dev, lba, buf, count) : -EOPNOTSUPP)
CLASS_BLOCK_OPERATION(class_block_write_sector,
    (block_dev_t *block, uint64_t lba, const void *buf, size_t count),
    ops->write ? ops->write(binding->dev, lba, buf, count) : -EOPNOTSUPP)
CLASS_BLOCK_OPERATION(class_block_write_batch,
    (block_dev_t *block, const block_write_t *writes, size_t count),
    ops->write_batch ? ops->write_batch(binding->dev, writes, count) : -EOPNOTSUPP)
CLASS_BLOCK_OPERATION(class_block_flush, (block_dev_t *block),
    ops->flush ? ops->flush(binding->dev) : -EOPNOTSUPP)
#undef CLASS_BLOCK_OPERATION

block_dev_t *mount_setup_block_device(int index)
{
    if (index < 0 || (unsigned)index >= CLASS_BLOCK_HANDLE_BUDGET) return NULL;
    class_device_t *binding = class_device_get_by_index(DEV_CLASS_BLOCK, (unsigned)index);
    if (!binding) return NULL;
    const void *table;
    if (class_device_ops_begin(binding, DEV_CLASS_BLOCK, &table)) {
        class_device_put(binding); return NULL;
    }
    const block_dev_ops_t *ops = table;
    int ready = ops->read && ops->write && ops->capacity && ops->sector_size;
    uint64_t capacity = ready ? ops->capacity(binding->dev) : 0;
    uint32_t sector_size = ready ? ops->sector_size(binding->dev) : 0;
    int has_batch = ready && ops->write_batch;
    class_device_call_end(binding);
    if (!ready || !capacity || !sector_size) { class_device_put(binding); return NULL; }
    class_block_dev_t *handle = &class_blocks[index];
    uint64_t flags = spin_lock_irqsave(&class_blocks_lock);
    if (handle->used) {
        int same = handle->binding == binding;
        spin_unlock_irqrestore(&class_blocks_lock, flags);
        class_device_put(binding);
        if (!same) printf("[BLOCK] stable handle index %d retired; descriptor budget exhausted for index\n", index);
        return same ? &handle->block : NULL;
    }
    handle->used = 1;
    class_device_get(binding);
    handle->binding = binding;
    handle->block = (block_dev_t){.read_sector = class_block_read_sector,
        .write_sector = class_block_write_sector, .flush = class_block_flush,
        .write_batch = has_batch ? class_block_write_batch : NULL,
        .capacity = capacity, .sector_size = sector_size, .priv = handle};
    spin_unlock_irqrestore(&class_blocks_lock, flags);
    int rc = class_device_set_offline(binding, class_block_offline, handle);
    if (rc) class_block_offline(binding, handle);
    class_device_put(binding);
    return rc ? NULL : &handle->block;
}

#if !defined(BRINGUP) || defined(CONFIG_STORAGE_READ_ONLY) || defined(CONFIG_RAMFS_MEDIA)
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

/* A flush is device-wide, not range-scoped, so a partition forwards it to the
 * whole-disk parent unchanged.  Omitting it here would silently strip the
 * durability guarantee from every filesystem mount, since all of them sit on
 * a partition. */
static int partition_flush(block_dev_t *block)
{
    partition_block_dev_t *part = (partition_block_dev_t *)block->priv;
    if (!part || !part->parent) return -ENODEV;
    if (part->read_only) return 0;
    if (!part->parent->flush) return -EOPNOTSUPP;
    return part->parent->flush(part->parent);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t read_le64(const uint8_t *p) {
    return read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}
static uint32_t gpt_crc32(uint32_t crc, const uint8_t *data, size_t bytes) {
    while (bytes--) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return crc;
}
/* Validate bounded GPT metadata before using partition offsets. Unsupported
 * logical sector geometry is refused before reading into 512-byte buffers.
 * The complete entry-array CRC prevents a valid header from hiding corrupt
 * partition LBAs. The boot policy examines the first sixteen entries. */
static block_dev_t *gpt_partition(block_dev_t *parent, int parent_index,
                                  int ordinal) {
    static partition_block_dev_t partitions[16][16];
    uint8_t header[512], sector[512], entry[128];
    if (!parent || parent_index < 0 || parent_index >= 16 || ordinal < 0 ||
        ordinal >= 16 || !parent->read_sector || parent->sector_size != 512 ||
        parent->capacity < 3 || parent->read_sector(parent, 1, header, 1))
        return NULL;
    if (memcmp(header, "EFI PART", 8) || read_le32(header + 8) != 0x10000)
        return NULL;
    uint32_t header_bytes = read_le32(header + 12), checksum = read_le32(header + 16);
    if (header_bytes < 92 || header_bytes > sizeof(header) ||
        read_le32(header + 20) || read_le64(header + 24) != 1 ||
        read_le64(header + 32) >= parent->capacity) return NULL;
    memset(header + 16, 0, 4);
    if (~gpt_crc32(~0U, header, header_bytes) != checksum) return NULL;
    uint64_t first_usable = read_le64(header + 40), last_usable = read_le64(header + 48);
    uint64_t entries_lba = read_le64(header + 72);
    uint32_t entry_count = read_le32(header + 80), entry_size = read_le32(header + 84);
    if (!entry_count || entry_count > 4096 || (unsigned)ordinal >= entry_count ||
        (entry_size != 128 && entry_size != 256 && entry_size != 512) ||
        first_usable < 3 || first_usable > last_usable || last_usable >= parent->capacity ||
        entries_lba < 2 || entries_lba >= parent->capacity) return NULL;
    uint64_t bytes = (uint64_t)entry_count * entry_size, sectors = (bytes + 511) / 512;
    if (sectors > parent->capacity - entries_lba ||
        entries_lba + sectors > first_usable) return NULL;
    uint32_t crc = ~0U;
    for (uint64_t at = 0; at < sectors; at++) {
        if (parent->read_sector(parent, entries_lba + at, sector, 1)) return NULL;
        size_t count = bytes > 512 ? 512 : (size_t)bytes;
        crc = gpt_crc32(crc, sector, count);
        bytes -= count;
    }
    if (~crc != read_le32(header + 88)) return NULL;
    uint64_t offset = (uint64_t)(unsigned)ordinal * entry_size;
    if (parent->read_sector(parent, entries_lba + offset / 512, sector, 1)) return NULL;
    memcpy(entry, sector + offset % 512, sizeof(entry));
    int used = 0;
    for (unsigned i = 0; i < 16; i++) used |= entry[i];
    uint64_t first = read_le64(entry + 32), last = read_le64(entry + 40);
    if (!used || first < first_usable || last < first || last > last_usable) return NULL;
    partition_block_dev_t *part = &partitions[parent_index][ordinal];
    *part = (partition_block_dev_t){.parent = parent, .first_lba = first,
                                   .sectors = last - first + 1};
    part->block = (block_dev_t){.read_sector = partition_read_sector,
        .write_sector = partition_write_sector, .flush = partition_flush,
        .capacity = part->sectors, .sector_size = parent->sector_size, .priv = part};
    return &part->block;
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
        partition.block.flush        = partition_flush;
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
    int ret = vfs_mount_bc_flags(mnt, fstype, bc, VFS_MOUNT_RDONLY, NULL);
    if (ret < 0) {
        bcache_destroy(bc);
        return ret;
    }
    printf("[STORAGE-RO] mounted %s read-only at %s\n", fstype, mnt);
    return 0;
}

/* Only FAT32 volumes are automatic media inputs. A read-only block view
 * supplements VFS_MOUNT_RDONLY: even a later remount cannot write the card. */
static int media_fat32(block_dev_t *disk) {
    uint8_t boot[512];
    if (!disk || !disk->read_sector || disk->sector_size != 512 ||
        disk->read_sector(disk, 0, boot, 1) || boot[510] != 0x55 || boot[511] != 0xaa ||
        boot[11] != 0 || boot[12] != 2 || boot[17] || boot[18] || boot[22] || boot[23])
        return 0;
    unsigned cluster = boot[13], fats = boot[16];
    uint64_t reserved = boot[14] | ((unsigned)boot[15] << 8);
    uint64_t fat_sectors = read_le32(boot + 36), total = read_le32(boot + 32);
    if (!cluster || cluster > 128 || (cluster & (cluster - 1)) || !reserved ||
        !fats || fats > 2 || !fat_sectors || !total || total > disk->capacity)
        return 0;
    uint64_t data = reserved + fats * fat_sectors;
    if (data >= total) return 0;
    uint64_t clusters = (total - data) / cluster;
    uint32_t root = read_le32(boot + 44);
    return clusters >= 65525 && root >= 2 && root - 2 < clusters &&
           fat_sectors * 128 >= clusters + 2;
}
static int mount_media_disk(block_dev_t *disk, int index) {
    static partition_block_dev_t whole[16];
    if (!disk || index < 0 || index >= 16 || disk->sector_size != 512) return -ENODEV;
    partition_block_dev_t *view = &whole[index];
    *view = (partition_block_dev_t){.parent = disk, .sectors = disk->capacity, .read_only = 1};
    view->block = (block_dev_t){.read_sector = partition_read_sector,
        .write_sector = partition_write_sector, .flush = partition_flush,
        .capacity = disk->capacity, .sector_size = 512, .priv = view};
    if (media_fat32(&view->block) && !try_mount_read_only(&view->block, "/media", "fat32"))
        return 0;
    for (int ordinal = 0; ordinal < 16; ordinal++) {
        block_dev_t *part = gpt_partition(disk, index, ordinal);
        if (!part) continue;
        ((partition_block_dev_t *)part->priv)->read_only = 1;
        if (media_fat32(part) && !try_mount_read_only(part, "/media", "fat32")) return 0;
    }
    return -ENODEV;
}
void mount_ramfs_media(void) {
    for (int index = 0; index < 16; index++) {
        block_dev_t *disk = mount_setup_block_device(index);
        if (!mount_media_disk(disk, index)) {
            printf("[MEDIA] disk %d FAT32 available read-only at /media; embedded /bin retained\n", index);
            return;
        }
    }
    printf("[MEDIA] no readable FAT32 media; RAM shell continues\n");
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

void mount_read_only_storage(void)
{
    printf("[INIT] read-only storage experiment is not enabled\n");
}
void mount_ramfs_media(void) { }

#endif /* !BRINGUP || CONFIG_STORAGE_READ_ONLY */
