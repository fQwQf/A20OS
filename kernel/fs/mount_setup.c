#include "core/stdio.h"
#include "core/string.h"
#include "core/errno.h"
#include "core/klog.h"
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
/* Disk index the distro rootfs was found on, so the log partition prefers the
 * same USB stick over an unrelated old A20OS log on the internal NVMe. */
static int g_distro_disk_index __attribute__((unused)) = -1;
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

#if !defined(BRINGUP) || defined(CONFIG_STORAGE_READ_ONLY) || defined(CONFIG_RAMFS_MEDIA) || defined(CONFIG_RAMFS_DISTRO)
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
/* A validated primary GPT is shared by both media and explicit distro boot.
 * Keep the complete entry-array CRC: a named partition is never accepted from
 * a corrupt table, and an entry beyond the legacy first-sixteen media slots
 * must still participate in uniqueness checks. */
typedef struct {
    uint64_t first_usable, last_usable, entries_lba;
    uint32_t entry_count, entry_size;
} gpt_table_t;

static int gpt_read_table(block_dev_t *parent, gpt_table_t *table) {
    uint8_t header[512], sector[512];
    if (!parent || !table || !parent->read_sector ||
        parent->sector_size != 512 || parent->capacity < 3 ||
        parent->read_sector(parent, 1, header, 1)) return -EINVAL;
    if (memcmp(header, "EFI PART", 8) || read_le32(header + 8) != 0x10000)
        return -EINVAL;
    uint32_t header_bytes = read_le32(header + 12), checksum = read_le32(header + 16);
    if (header_bytes < 92 || header_bytes > sizeof(header) ||
        read_le32(header + 20) || read_le64(header + 24) != 1 ||
        read_le64(header + 32) >= parent->capacity) return -EINVAL;
    memset(header + 16, 0, 4);
    if (~gpt_crc32(~0U, header, header_bytes) != checksum) return -EINVAL;
    *table = (gpt_table_t){.first_usable = read_le64(header + 40),
        .last_usable = read_le64(header + 48), .entries_lba = read_le64(header + 72),
        .entry_count = read_le32(header + 80), .entry_size = read_le32(header + 84)};
    if (!table->entry_count || table->entry_count > 4096 ||
        (table->entry_size != 128 && table->entry_size != 256 && table->entry_size != 512) ||
        table->first_usable < 3 || table->first_usable > table->last_usable ||
        table->last_usable >= parent->capacity || table->entries_lba < 2 ||
        table->entries_lba >= parent->capacity) return -EINVAL;
    uint64_t bytes = (uint64_t)table->entry_count * table->entry_size;
    uint64_t sectors = (bytes + 511) / 512;
    uint64_t backup = read_le64(header + 32);
    if (sectors > parent->capacity - table->entries_lba ||
        table->entries_lba + sectors > table->first_usable ||
        sectors >= backup || table->last_usable >= backup - sectors) return -EINVAL;
    uint32_t crc = ~0U;
    for (uint64_t at = 0; at < sectors; at++) {
        if (parent->read_sector(parent, table->entries_lba + at, sector, 1)) return -EIO;
        size_t count = bytes > 512 ? 512 : (size_t)bytes;
        crc = gpt_crc32(crc, sector, count);
        bytes -= count;
    }
    return ~crc == read_le32(header + 88) ? 0 : -EINVAL;
}

static int gpt_read_entry(block_dev_t *parent, const gpt_table_t *table,
                           unsigned ordinal, uint8_t entry[128]) {
    uint8_t sector[512];
    if (ordinal >= table->entry_count) return -EINVAL;
    uint64_t offset = (uint64_t)ordinal * table->entry_size;
    if (parent->read_sector(parent, table->entries_lba + offset / 512, sector, 1))
        return -EIO;
    memcpy(entry, sector + offset % 512, 128);
    return 0;
}

static int gpt_entry_used(const uint8_t entry[128]) {
    unsigned used = 0;
    for (unsigned i = 0; i < 16; i++) used |= entry[i];
    return used != 0;
}
static int gpt_entry_bounds(const gpt_table_t *table, const uint8_t entry[128]) {
    uint64_t first = read_le64(entry + 32), last = read_le64(entry + 40);
    return first >= table->first_usable && last >= first && last <= table->last_usable;
}
static void partition_init(partition_block_dev_t *part, block_dev_t *parent,
                            uint64_t first, uint64_t sectors, int read_only) {
    *part = (partition_block_dev_t){.parent = parent, .first_lba = first,
                                  .sectors = sectors, .read_only = read_only};
    part->block = (block_dev_t){.read_sector = partition_read_sector,
        .write_sector = partition_write_sector, .flush = partition_flush,
        .capacity = sectors, .sector_size = parent->sector_size, .priv = part};
}
static block_dev_t *gpt_partition(block_dev_t *parent, int parent_index,
                                  int ordinal) {
    static partition_block_dev_t partitions[16][16];
    uint8_t entry[128];
    gpt_table_t table;
    if (parent_index < 0 || parent_index >= 16 || ordinal < 0 || ordinal >= 16 ||
        gpt_read_table(parent, &table) ||
        gpt_read_entry(parent, &table, (unsigned)ordinal, entry) ||
        !gpt_entry_used(entry) || !gpt_entry_bounds(&table, entry)) return NULL;
    partition_block_dev_t *part = &partitions[parent_index][ordinal];
    uint64_t first = read_le64(entry + 32), last = read_le64(entry + 40);
    partition_init(part, parent, first, last - first + 1, 0);
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

/*
 * Persistent log partition.
 *
 * A dedicated FAT32 GPT partition named "A20OS log" is the one writable
 * surface whose lifetime is independent of the distro rootfs.  It is mounted
 * before chroot (at /extra/var/log/a20 when a distro is present, otherwise at
 * /var/log/a20) so a corrupt or unmountable world cannot take the boot log
 * with it.  Matching by GPT name, not by filesystem type, is intentional:
 * parted labels a FAT32 data partition with a different type GUID than an
 * ext4 one, and the name is what the image builder and the docs agree on.
 */
#if CONFIG_KLOG_PERSISTENT
static int log_partition_name(const uint8_t entry[128]) {
    static const char name[] = "A20OS log";
    for (unsigned i = 0; i < 36; i++) {
        unsigned expected = i < sizeof(name) - 1 ? (unsigned char)name[i] : 0;
        if (entry[56 + i * 2] != expected || entry[57 + i * 2]) return 0;
    }
    return 1;
}
static int log_disk_partition(block_dev_t *disk,
                              partition_block_dev_t *selected) {
    gpt_table_t table;
    uint8_t entry[128], candidate[128];
    if (gpt_read_table(disk, &table)) return -EINVAL;
    unsigned found = 0, candidate_ordinal = 0;
    for (unsigned ordinal = 0; ordinal < table.entry_count; ordinal++) {
        if (gpt_read_entry(disk, &table, ordinal, entry)) return -EIO;
        if (!gpt_entry_used(entry)) continue;
        if (!gpt_entry_bounds(&table, entry)) return -EINVAL;
        if (!log_partition_name(entry)) continue;
        if (++found != 1) return -EEXIST;
        memcpy(candidate, entry, sizeof(candidate));
        candidate_ordinal = ordinal;
    }
    if (!found) return -ENODEV;
    if (read_le64(candidate + 48) & ((1ULL << 60) | (1ULL << 63)))
        return -EROFS;
    uint64_t first = read_le64(candidate + 32), last = read_le64(candidate + 40);
    for (unsigned ordinal = 0; ordinal < table.entry_count; ordinal++) {
        if (gpt_read_entry(disk, &table, ordinal, entry)) return -EIO;
        if (!gpt_entry_used(entry)) continue;
        if (ordinal == candidate_ordinal) continue;
        uint64_t other_first = read_le64(entry + 32);
        uint64_t other_last = read_le64(entry + 40);
        if (first <= other_last && other_first <= last) return -EINVAL;
    }
    partition_init(selected, disk, first, last - first + 1, 0);
    return 0;
}

/* EFI System Partition type GUID, in the mixed-endian GPT wire form.  Used
 * only as a fallback when no dedicated "A20OS log" partition exists: an
 * internal ESP is the one writable FAT32 surface that does not share the USB
 * xHCI failure.  Writes stay in its \A20LOG directory. */
static const uint8_t esp_type_guid[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
};

static int esp_disk_partition(block_dev_t *disk,
                              partition_block_dev_t *selected) {
    gpt_table_t table;
    uint8_t entry[128], candidate[128];
    if (gpt_read_table(disk, &table)) return -EINVAL;
    unsigned found = 0, candidate_ordinal = 0;
    for (unsigned ordinal = 0; ordinal < table.entry_count; ordinal++) {
        if (gpt_read_entry(disk, &table, ordinal, entry)) return -EIO;
        if (!gpt_entry_used(entry)) continue;
        if (!gpt_entry_bounds(&table, entry)) return -EINVAL;
        if (memcmp(entry, esp_type_guid, sizeof(esp_type_guid))) continue;
        if (++found != 1) return -EEXIST;
        memcpy(candidate, entry, sizeof(candidate));
        candidate_ordinal = ordinal;
    }
    if (!found) return -ENODEV;
    if (read_le64(candidate + 48) & ((1ULL << 60) | (1ULL << 63)))
        return -EROFS;
    uint64_t first = read_le64(candidate + 32), last = read_le64(candidate + 40);
    for (unsigned ordinal = 0; ordinal < table.entry_count; ordinal++) {
        if (gpt_read_entry(disk, &table, ordinal, entry)) return -EIO;
        if (!gpt_entry_used(entry)) continue;
        if (ordinal == candidate_ordinal) continue;
        uint64_t other_first = read_le64(entry + 32);
        uint64_t other_last = read_le64(entry + 40);
        if (first <= other_last && other_first <= last) return -EINVAL;
    }
    partition_init(selected, disk, first, last - first + 1, 0);
    return 0;
}

/* Do not write boot evidence to the stick whose xHCI controller is the thing
 * under diagnosis.  Every other block driver at this point is internal
 * storage (NVMe, AHCI, virtio, SD). */
static int block_is_internal(int index) {
    class_device_t *cdev =
        class_device_get_by_index(DEV_CLASS_BLOCK, (unsigned)index);
    if (!cdev)
        return 0;
    const char *driver = (cdev->dev && cdev->dev->drv) ?
                         cdev->dev->drv->name : NULL;
    int internal = driver &&
                   strcmp(driver, "usb-storage") != 0 &&
                   strcmp(driver, "udisk") != 0;
    class_device_put(cdev);
    return internal;
}

static void ensure_dir(const char *path) {
    char tmp[160];
    if (!path || strlen(path) >= sizeof(tmp))
        return;
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        (void)vfs_mkdir(tmp, 0755);
        *p = '/';
    }
    (void)vfs_mkdir(tmp, 0755);
}

static int path_is_mount(const char *path) {
    kstat_t here, root;
    if (vfs_stat(path, &here) < 0 || vfs_stat("/", &root) < 0)
        return 0;
    return here.st_dev != root.st_dev;
}

/* Where a rootfs-visible log directory belongs.  /mnt is the external-root
 * layout, /extra is the RAMFS distro layout; "" means the RAM root. */
static const char *log_root_prefix(void) {
    int fd = vfs_open("/mnt/etc/a20-distro", O_RDONLY, 0);
    if (fd >= 0) {
        vfs_close(fd);
        return "/mnt";
    }
    fd = vfs_open("/extra/etc/a20-distro", O_RDONLY, 0);
    if (fd >= 0) {
        vfs_close(fd);
        return "/extra";
    }
    if (path_is_mount("/mnt"))
        return "/mnt";
    if (path_is_mount("/extra"))
        return "/extra";
    return "";
}

int mount_persistent_log(void) {
    if (!klog_persistent_enabled())
        return -EOPNOTSUPP;
    int mode = klog_mode();
    if (mode == KLOG_MODE_RAM) {
        ensure_dir("/var/log/a20");
        if (klog_disk_attach("/var/log/a20") == 0) {
            printf("[LOG] RAM log at /var/log/a20 (volatile, RAM mode)\n");
            return 0;
        }
        printf("[LOG] RAM log location unavailable\n");
        return -ENODEV;
    }

    static partition_block_dev_t logpart;
    static partition_block_dev_t esp_logpart;
    partition_block_dev_t candidate;
    int selected_index = -1;
    int use_log_partition = mode == KLOG_MODE_AUTO || mode == KLOG_MODE_PARTITION;

    /* Prefer the disk the distro came from.  A laptop may still carry an old
     * A20OS log partition on its internal NVMe; the USB being booted is the
     * authoritative evidence surface, so a second one elsewhere is not an
     * ambiguity here. */
    if (use_log_partition && g_distro_disk_index >= 0) {
        block_dev_t *disk = mount_setup_block_device(g_distro_disk_index);
        if (disk && log_disk_partition(disk, &candidate) == 0) {
            logpart = candidate;
            logpart.block.priv = &logpart;
            selected_index = g_distro_disk_index;
        }
    }
    if (use_log_partition && selected_index < 0) {
        for (unsigned index = 0; index < CLASS_BLOCK_HANDLE_BUDGET; index++) {
            block_dev_t *disk = mount_setup_block_device((int)index);
            if (!disk) continue;
            int rc = log_disk_partition(disk, &candidate);
            if (rc == -EEXIST) {
                printf("[LOG] ambiguous A20OS log partitions; not mounting\n");
                return rc;
            }
            if (rc) continue;
            if (selected_index >= 0) {
                printf("[LOG] multiple A20OS log disks; not mounting\n");
                return -EEXIST;
            }
            logpart = candidate;
            logpart.block.priv = &logpart;
            selected_index = (int)index;
        }
    }

    /* Keep USB failures off the kernel's evidence path when the boot medium
     * is unavailable or explicitly excluded as a log destination. */
    if (mode == KLOG_MODE_ESP ||
        (mode == KLOG_MODE_AUTO && selected_index < 0)) {
        for (unsigned index = 0; index < CLASS_BLOCK_HANDLE_BUDGET; index++) {
            if (!block_is_internal((int)index))
                continue;
            block_dev_t *disk = mount_setup_block_device((int)index);
            if (!disk)
                continue;
            if (esp_disk_partition(disk, &esp_logpart) != 0)
                continue;
            ensure_dir("/var/log/a20-esp");
            int rc = try_mount(&esp_logpart.block, "/var/log/a20-esp", "fat32");
            if (rc != 0) {
                printf("[LOG] internal ESP disk %u mount failed (%d)\n",
                       index, rc);
                continue;
            }
            ensure_dir("/var/log/a20-esp/A20LOG");
            if (klog_disk_attach("/var/log/a20-esp/A20LOG") == 0) {
                printf("[LOG] persistent log at /var/log/a20-esp/A20LOG "
                       "(internal ESP disk %u)\n", index);
                return 0;
            }
            printf("[LOG] internal ESP disk %u: log create failed\n", index);
            break;
        }
        if (mode == KLOG_MODE_ESP) {
            printf("[LOG] no writable internal ESP for A20LOG\n");
            return -ENODEV;
        }
    }

    const char *prefix = log_root_prefix();
    char mnt[160];
    if (prefix[0])
        snprintf(mnt, sizeof(mnt), "%s/var/log/a20", prefix);
    else
        snprintf(mnt, sizeof(mnt), "/var/log/a20");

    if (selected_index >= 0) {
        ensure_dir(mnt);
        int rc = try_mount(&logpart.block, mnt, "fat32");
        if (rc == 0) {
            printf("[LOG] persistent log at %s (disk %d)\n", mnt,
                   selected_index);
            if (klog_disk_attach(mnt) == 0)
                return 0;
            printf("[LOG] log file creation failed at %s\n", mnt);
        } else {
            printf("[LOG] FAT32 mount for log partition failed (%d)\n", rc);
        }
    } else if (mode == KLOG_MODE_ROOTFS) {
        printf("[LOG] rootfs mode: skipping the dedicated partition\n");
    } else if (mode == KLOG_MODE_PARTITION) {
        printf("[LOG] no A20OS log partition and mode=partition\n");
        return -ENODEV;
    } else {
        printf("[LOG] no A20OS log partition; trying a writable filesystem\n");
    }

    /* Fallback for QEMU/SD images that have no dedicated partition.  A
     * dedicated FAT32 partition is still preferred: this path shares the
     * rootfs's failure and capacity, but it is better than no log at all.
     * Only try a root that actually exists; creating /mnt on the RAM root
     * would silently turn the log volatile. */
    if (prefix[0]) {
        ensure_dir(mnt);
        if (klog_disk_attach(mnt) == 0) {
            printf("[LOG] %s log at %s (rootfs fallback)\n", klog_disk_storage(), mnt);
            return 0;
        }
    }
    if (path_is_mount("/bin")) {
        ensure_dir("/bin/A20OS-LOG");
        if (klog_disk_attach("/bin/A20OS-LOG") == 0) {
            printf("[LOG] persistent log at /bin/A20OS-LOG (rootfs fallback)\n");
            return 0;
        }
    }
    ensure_dir("/var/log/a20");
    if (klog_disk_attach("/var/log/a20") == 0) {
        printf("[LOG] RAM log at /var/log/a20 (volatile, RAM fallback)\n");
        return 0;
    }
    printf("[LOG] no writable log location; persistent log unavailable\n");
    return -ENODEV;
}
#else
int mount_persistent_log(void) { return -EOPNOTSUPP; }
#endif

#ifdef CONFIG_RAMFS_DISTRO
static void mount_external_root_pseudo_filesystems(void);
/* Linux filesystem data GUID, in the mixed-endian GPT wire representation. */
static const uint8_t distro_type_guid[16] = {
    0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4
};
static int distro_partition_name(const uint8_t entry[128]) {
    static const char name[] = "A20OS distro";
    for (unsigned i = 0; i < 36; i++) {
        unsigned expected = i < sizeof(name) - 1 ? (unsigned char)name[i] : 0;
        if (entry[56 + i * 2] != expected || entry[57 + i * 2]) return 0;
    }
    return 1;
}
static int distro_disk_partition(block_dev_t *disk, partition_block_dev_t *selected) {
    gpt_table_t table;
    uint8_t entry[128], candidate[128];
    if (gpt_read_table(disk, &table)) return -EINVAL;
    unsigned found = 0, candidate_ordinal = 0;
    /* Scan the complete bounded table, not just the media parser's 16 slots. */
    for (unsigned ordinal = 0; ordinal < table.entry_count; ordinal++) {
        if (gpt_read_entry(disk, &table, ordinal, entry)) return -EIO;
        if (!gpt_entry_used(entry)) continue;
        if (!gpt_entry_bounds(&table, entry)) return -EINVAL;
        if (memcmp(entry, distro_type_guid, 16) || !distro_partition_name(entry)) continue;
        if (++found != 1) return -EEXIST;
        memcpy(candidate, entry, sizeof(candidate));
        candidate_ordinal = ordinal;
    }
    if (!found) return -ENODEV;
    /* Respect the Linux data type's read-only and no-auto-mount attributes. */
    if (read_le64(candidate + 48) & ((1ULL << 60) | (1ULL << 63))) return -EROFS;
    uint64_t first = read_le64(candidate + 32), last = read_le64(candidate + 40);
    /* A valid checksum is insufficient if a named entry aliases another
     * partition. Never expose an overlapping Windows/ESP range as writable. */
    for (unsigned ordinal = 0; ordinal < table.entry_count; ordinal++) {
        if (gpt_read_entry(disk, &table, ordinal, entry)) return -EIO;
        if (!gpt_entry_used(entry)) continue;
        if (ordinal == candidate_ordinal) continue;
        uint64_t other_first = read_le64(entry + 32), other_last = read_le64(entry + 40);
        if (first <= other_last && other_first <= last) return -EINVAL;
    }
    partition_init(selected, disk, first, last - first + 1, 0);
    return 0;
}

int mount_ramfs_distro(void) {
    /* Separate ownership from the media parser's reusable views: a later
     * /media scan must never rewrite a mounted distro's block offsets/flags. */
    static partition_block_dev_t distro;
    partition_block_dev_t candidate;
    int selected_index = -1;
    for (unsigned index = 0; index < CLASS_BLOCK_HANDLE_BUDGET; index++) {
        block_dev_t *disk = mount_setup_block_device((int)index);
        if (!disk) continue;
        int rc = distro_disk_partition(disk, &candidate);
        if (rc == -EEXIST) {
            printf("[DISTRO] ambiguous A20OS GPT partitions; RAM shell continues\n");
            return rc;
        }
        if (rc) continue;
        if (selected_index >= 0) {
            printf("[DISTRO] multiple A20OS distro disks; RAM shell continues\n");
            return -EEXIST;
        }
        distro = candidate;
        distro.block.priv = &distro;
        selected_index = (int)index;
    }
    if (selected_index < 0) {
        printf("[DISTRO] no unique A20OS distro GPT partition; RAM shell continues\n");
        return -ENODEV;
    }
    int rc = try_mount(&distro.block, "/extra", "ext4");
    if (rc) {
        printf("[DISTRO] ext4 mount failed (%d); RAM shell continues\n", rc);
        return rc;
    }
    g_distro_disk_index = selected_index;
    printf("[DISTRO] disk %d A20OS distro at /extra; embedded /bin retained\n", selected_index);
    mount_external_root_pseudo_filesystems();
    return 0;
}
#else
int mount_ramfs_distro(void) { return -EOPNOTSUPP; }
#endif

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
int mount_ramfs_distro(void) { return -EOPNOTSUPP; }

#endif /* !BRINGUP || CONFIG_STORAGE_READ_ONLY */
