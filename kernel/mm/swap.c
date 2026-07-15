#include "mm/swap.h"

#ifdef CONFIG_SWAP

#include "core/consts.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/random.h"
#include "core/string.h"
#include "drivers/block/block_dev.h"
#include "mm/slab.h"

#define SWAP_SECTOR_SIZE 512UL
#define SWAP_SECTORS_PER_PAGE (PAGE_SIZE / SWAP_SECTOR_SIZE)
#define SWP_TYPE_MASK ((1ULL << SWP_TYPE_BITS) - 1)
#define SWP_OFFSET_MASK ((1ULL << SWP_OFFSET_BITS) - 1)

swap_info_struct swap_info[MAX_SWAPFILES];
size_t total_swap_pages;
size_t nr_swap_pages;
static spinlock_t swap_locks[MAX_SWAPFILES];
static spinlock_t swap_stats_lock;

static int swap_device_valid(block_dev_t *bdev)
{
    return bdev && bdev->read_sector && bdev->write_sector &&
           bdev->sector_size == SWAP_SECTOR_SIZE;
}

static size_t swap_badmap_bytes(uint64_t pages)
{
    return (size_t)((pages + 7) / 8);
}

static uint64_t swap_header_pages(uint64_t pages)
{
    return 1 + (swap_badmap_bytes(pages) + PAGE_SIZE - 1) / PAGE_SIZE;
}

static void swap_badmap_set(uint8_t *badmap, uint64_t page)
{
    badmap[page / 8] |= (uint8_t)(1U << (page % 8));
}

static int swap_badmap_test(const uint8_t *badmap, uint64_t page)
{
    return badmap[page / 8] & (uint8_t)(1U << (page % 8));
}

static inline unsigned int swp_type(swap_entry_t entry) {
    return (unsigned int)(entry & SWP_TYPE_MASK);
}

static inline uint64_t swp_offset(swap_entry_t entry) {
    return (entry >> SWP_TYPE_BITS) & SWP_OFFSET_MASK;
}

static inline swap_entry_t swp_entry(unsigned int type, uint64_t offset) {
    return ((offset & SWP_OFFSET_MASK) << SWP_TYPE_BITS) | type;
}

void swap_init(void) {
    memset(swap_info, 0, sizeof(swap_info));
    total_swap_pages = 0;
    nr_swap_pages = 0;
    spin_init(&swap_stats_lock);
    for (int i = 0; i < MAX_SWAPFILES; i++)
        spin_init(&swap_locks[i]);
}

int swap_register_device(block_dev_t *bdev, const char *name) {
    if (!name || !swap_device_valid(bdev))
        return -EINVAL;

    uint8_t *header_page = kmalloc(PAGE_SIZE);
    if (!header_page)
        return -ENOMEM;
    int ret = bdev->read_sector(bdev, 0, header_page, SWAP_SECTORS_PER_PAGE);
    if (ret < 0) {
        kfree(header_page);
        return ret;
    }

    struct swap_header *header = (struct swap_header *)header_page;
    uint64_t device_pages = bdev->capacity / SWAP_SECTORS_PER_PAGE;
    uint64_t pages = header->pages;
    if (memcmp(header->magic, SWAP_MAGIC, sizeof(header->magic)) != 0 ||
        header->version != SWAP_VERSION || pages == 0 || pages > device_pages ||
        pages > SWP_OFFSET_MASK) {
        kfree(header_page);
        return -EINVAL;
    }

    uint64_t header_pages = swap_header_pages(pages);
    if (header_pages > pages) {
        kfree(header_page);
        return -EINVAL;
    }

    size_t badmap_storage = (size_t)(header_pages - 1) * PAGE_SIZE;
    uint8_t *badmap = kmalloc(badmap_storage);
    if (!badmap) {
        kfree(header_page);
        return -ENOMEM;
    }
    memset(badmap, 0, badmap_storage);
    for (uint64_t page = 1; page < header_pages; page++) {
        ret = bdev->read_sector(bdev, page * SWAP_SECTORS_PER_PAGE,
                                badmap + (page - 1) * PAGE_SIZE,
                                SWAP_SECTORS_PER_PAGE);
        if (ret < 0) {
            kfree(badmap);
            kfree(header_page);
            return ret;
        }
    }
    kfree(header_page);

    for (int type = 0; type < MAX_SWAPFILES; type++) {
        swap_info_struct *si = &swap_info[type];
        uint64_t flags = spin_lock_irqsave(&swap_locks[type]);
        if (si->active) {
            spin_unlock_irqrestore(&swap_locks[type], flags);
            continue;
        }
        spin_unlock_irqrestore(&swap_locks[type], flags);

        uint8_t *map = kmalloc((size_t)pages);
        if (!map) {
            kfree(badmap);
            return -ENOMEM;
        }
        size_t name_len = strlen(name) + 1;
        char *name_copy = kmalloc(name_len);
        if (!name_copy) {
            kfree(map);
            kfree(badmap);
            return -ENOMEM;
        }
        memcpy(name_copy, name, name_len);
        memset(map, 0, (size_t)pages);
        size_t available_pages = 0;
        for (uint64_t offset = 0; offset < pages; offset++) {
            if (swap_badmap_test(badmap, offset))
                map[offset] = SWAP_MAP_BAD;
            else
                available_pages++;
        }
        flags = spin_lock_irqsave(&swap_locks[type]);
        if (si->active) {
            spin_unlock_irqrestore(&swap_locks[type], flags);
            kfree(map);
            kfree(name_copy);
            continue;
        }
        kfree(badmap);
        si->bdev = bdev;
        si->name = name_copy;
        si->swap_map = map;
        si->pages = pages;
        si->inuse_pages = 0;
        si->active = 1;
        uint64_t stats_flags = spin_lock_irqsave(&swap_stats_lock);
        total_swap_pages += available_pages;
        nr_swap_pages += available_pages;
        spin_unlock_irqrestore(&swap_stats_lock, stats_flags);
        spin_unlock_irqrestore(&swap_locks[type], flags);
        return type;
    }
    kfree(badmap);
    return -ENOSPC;
}

int swap_format_device(block_dev_t *bdev, const char *name, int priority,
                       const char *label)
{
    if (!swap_device_valid(bdev))
        return -EINVAL;

    uint64_t pages = bdev->capacity / SWAP_SECTORS_PER_PAGE;
    if (pages == 0 || pages > SWP_OFFSET_MASK)
        return -EINVAL;

    uint64_t header_pages = swap_header_pages(pages);
    if (header_pages > pages)
        return -EINVAL;

    size_t header_bytes = (size_t)header_pages * PAGE_SIZE;
    uint8_t *buffer = kmalloc(header_bytes);
    if (!buffer)
        return -ENOMEM;
    memset(buffer, 0, header_bytes);

    struct swap_header *header = (struct swap_header *)buffer;
    memcpy(header->magic, SWAP_MAGIC, sizeof(header->magic));
    header->version = SWAP_VERSION;
    header->pages = pages;
    header->bad_pages = (uint32_t)header_pages;
    header->priority = priority;
    if (label)
        strncpy(header->label, label, sizeof(header->label) - 1);
    else if (name)
        strncpy(header->label, name, sizeof(header->label) - 1);
    random_fill(header->uuid, sizeof(header->uuid));
    for (uint64_t page = 0; page < header_pages; page++)
        swap_badmap_set(header->badmap, page);

    int ret = bdev->write_sector(bdev, 0, buffer,
                                 header_pages * SWAP_SECTORS_PER_PAGE);
    kfree(buffer);
    return ret;
}

void swap_unregister_device(int type) {
    if (type < 0 || type >= MAX_SWAPFILES)
        return;

    swap_info_struct *si = &swap_info[type];
    uint64_t flags = spin_lock_irqsave(&swap_locks[type]);
    uint8_t *map = si->swap_map;
    const char *name = si->name;
    if (!si->active) {
        spin_unlock_irqrestore(&swap_locks[type], flags);
        return;
    }
    size_t total_pages = 0;
    size_t free_pages = 0;
    for (uint64_t offset = 0; offset < si->pages; offset++) {
        if (si->swap_map[offset] != SWAP_MAP_BAD)
            total_pages++;
        if (si->swap_map[offset] == 0)
            free_pages++;
    }
    uint64_t stats_flags = spin_lock_irqsave(&swap_stats_lock);
    total_swap_pages -= total_pages;
    nr_swap_pages -= free_pages;
    spin_unlock_irqrestore(&swap_stats_lock, stats_flags);
    si->bdev = NULL;
    si->name = NULL;
    si->swap_map = NULL;
    si->pages = 0;
    si->inuse_pages = 0;
    si->active = 0;
    spin_unlock_irqrestore(&swap_locks[type], flags);
    kfree(map);
    kfree((void *)name);
}

swap_entry_t get_swap_page(void) {
    for (unsigned int type = 0; type < MAX_SWAPFILES; type++) {
        swap_info_struct *si = &swap_info[type];
        uint64_t flags = spin_lock_irqsave(&swap_locks[type]);
        if (si->active) {
            for (uint64_t offset = 1; offset < si->pages; offset++) {
                if (si->swap_map[offset] == 0) {
                    si->swap_map[offset] = 1;
                    uint64_t stats_flags = spin_lock_irqsave(&swap_stats_lock);
                    nr_swap_pages--;
                    si->inuse_pages++;
                    spin_unlock_irqrestore(&swap_stats_lock, stats_flags);
                    spin_unlock_irqrestore(&swap_locks[type], flags);
                    return swp_entry(type, offset);
                }
            }
        }
        spin_unlock_irqrestore(&swap_locks[type], flags);
    }
    return 0;
}

void swap_free(swap_entry_t entry) {
    unsigned int type = swp_type(entry);
    uint64_t offset = swp_offset(entry);
    if (type >= MAX_SWAPFILES)
        return;

    swap_info_struct *si = &swap_info[type];
    uint64_t flags = spin_lock_irqsave(&swap_locks[type]);
    if (si->active && offset > 0 && offset < si->pages &&
        si->swap_map[offset] == 1) {
        si->swap_map[offset] = 0;
        uint64_t stats_flags = spin_lock_irqsave(&swap_stats_lock);
        si->inuse_pages--;
        nr_swap_pages++;
        spin_unlock_irqrestore(&swap_stats_lock, stats_flags);
    }
    spin_unlock_irqrestore(&swap_locks[type], flags);
}

static int swap_page_io(swap_entry_t entry, void *page, int write) {
    unsigned int type = swp_type(entry);
    uint64_t offset = swp_offset(entry);
    if (!page || type >= MAX_SWAPFILES)
        return -EINVAL;

    swap_info_struct *si = &swap_info[type];
    uint64_t flags = spin_lock_irqsave(&swap_locks[type]);
    block_dev_t *bdev = si->bdev;
    int valid = si->active && bdev && offset < si->pages &&
                si->swap_map[offset] == 1;
    spin_unlock_irqrestore(&swap_locks[type], flags);
    if (!valid)
        return -EINVAL;

    uint64_t lba = offset * SWAP_SECTORS_PER_PAGE;
    if (write)
        return bdev->write_sector(bdev, lba, page, SWAP_SECTORS_PER_PAGE);
    return bdev->read_sector(bdev, lba, page, SWAP_SECTORS_PER_PAGE);
}

int swap_read_page(swap_entry_t entry, void *page) {
    return swap_page_io(entry, page, 0);
}

int swap_write_page(swap_entry_t entry, const void *page) {
    return swap_page_io(entry, (void *)page, 1);
}

#endif
