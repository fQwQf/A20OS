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

/*
 * Allocation index, one word per 64 slots of an area's swap_map, with a bit set
 * for every slot that is free and not bad.  swap_map alone is one byte per page,
 * so scanning it for a free slot is O(pages) of byte loads under the area lock
 * and gets steadily worse as the area fills.  The bitmap makes a scan 64x
 * cheaper and the hint makes the common case O(1): allocation continues from
 * where the last one stopped instead of restarting at the start of the area.
 *
 * This is an index over swap_map, never a second source of truth: every bit
 * transition here is paired with the swap_map store beside it, and swap_map
 * remains what the I/O path validates against.
 */
static uint64_t *swap_free_bits[MAX_SWAPFILES];
static uint64_t  swap_alloc_hint[MAX_SWAPFILES];

/*
 * Transfers holding a reference to an area's block device without holding the
 * area lock, and the device captured by swap_unregister_device() awaiting
 * release once they drain.  swap_page_io() drops the lock before driving the
 * device because the I/O can block, so swap_unregister_device() cannot know on
 * its own that nobody is still inside the driver.  The pending slot is cleared
 * under the lock by whichever of the two finds the count at zero, so the
 * release happens exactly once and never on a device still being driven.
 */
static unsigned int  swap_io_refs[MAX_SWAPFILES];
static block_dev_t  *swap_pending_bdev[MAX_SWAPFILES];

static inline void swap_bit_set(uint64_t *bits, uint64_t i)
{
    bits[i >> 6] |= (uint64_t)1 << (i & 63);
}

static inline void swap_bit_clear(uint64_t *bits, uint64_t i)
{
    bits[i >> 6] &= ~((uint64_t)1 << (i & 63));
}

static size_t swap_bitmap_words(uint64_t pages)
{
    return (size_t)((pages + 63) / 64);
}

/*
 * Index of the highest set bit, without __builtin_clzll: that expands to a
 * libgcc call (__clzdi2) which this freestanding link does not pull in.
 */
static inline uint64_t swap_highest_bit(uint64_t word)
{
    uint64_t bit = 0;
    if (word & 0xFFFFFFFF00000000ull) { word >>= 32; bit += 32; }
    if (word & 0x00000000FFFF0000ull) { word >>= 16; bit += 16; }
    if (word & 0x000000000000FF00ull) { word >>= 8;  bit += 8; }
    if (word & 0x00000000000000F0ull) { word >>= 4;  bit += 4; }
    if (word & 0x000000000000000Cull) { word >>= 2;  bit += 2; }
    if (word & 0x0000000000000002ull) { bit += 1; }
    return bit;
}

/*
 * Find a free slot at or after `hint`, wrapping to the start of the area once
 * the tail is exhausted.  Scanning whole words is what keeps this cheap; the
 * hint keeps it from re-walking the allocated prefix on every call.
 */
static int swap_bitmap_find_free(swap_info_struct *si, int type,
                                 uint64_t *out)
{
    uint64_t *bits = swap_free_bits[type];
    uint64_t pages = si->pages;
    uint64_t hint = swap_alloc_hint[type];

    /* Slot 0 is the header and is never allocatable. */
    if (hint < 1) hint = 1;
    if (hint >= pages) hint = 1;

    for (int pass = 0; pass < 2; pass++) {
        uint64_t start = pass == 0 ? hint : 1;
        uint64_t end   = pass == 0 ? pages : hint;
        uint64_t first_w = start >> 6;
        uint64_t last_w  = (end + 63) >> 6;
        for (uint64_t w = first_w; w < last_w; w++) {
            uint64_t word = bits[w];
            if (!word)
                continue;
            /* Drop the bits before `start` in the word `start` lands in. */
            if (w == first_w && (start & 63))
                word &= ~(((uint64_t)1 << (start & 63)) - 1);
            if (!word)
                continue;
            uint64_t bit = swap_highest_bit(word);
            uint64_t idx = (w << 6) + bit;
            if (idx < start || idx >= end || idx >= pages || idx == 0)
                continue;
            *out = idx;
            return 1;
        }
    }
    return 0;
}

/*
 * Claim the pending device if no transfer is still in flight, and return it
 * unlocked so the caller can call ->release() outside the area lock.  Only one
 * of swap_page_io() and swap_unregister_device() can win the claim.
 */
static block_dev_t *swap_take_pending_bdev(int type)
{
    uint64_t flags = spin_lock_irqsave(&swap_locks[type]);
    block_dev_t *bdev = NULL;
    if (swap_io_refs[type] == 0) {
        bdev = swap_pending_bdev[type];
        swap_pending_bdev[type] = NULL;
    }
    spin_unlock_irqrestore(&swap_locks[type], flags);
    return bdev;
}

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
    for (int i = 0; i < MAX_SWAPFILES; i++) {
        spin_init(&swap_locks[i]);
        swap_free_bits[i] = NULL;
        swap_pending_bdev[i] = NULL;
        swap_alloc_hint[i] = 1;
        swap_io_refs[i] = 0;
    }
}

int swap_register_device(block_dev_t *bdev, const char *name) {
    if (!name || !swap_device_valid(bdev))
        return -EINVAL;

    /* Linux rejects a second swapon of an already-active device. */
    for (int type = 0; type < MAX_SWAPFILES; type++) {
        uint64_t flags = spin_lock_irqsave(&swap_locks[type]);
        int dup = swap_info[type].active && swap_info[type].bdev == bdev;
        spin_unlock_irqrestore(&swap_locks[type], flags);
        if (dup)
            return -EBUSY;
    }

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
        uint64_t *bits = kmalloc(swap_bitmap_words(pages) * sizeof(uint64_t));
        if (!bits) {
            kfree(map);
            kfree(badmap);
            return -ENOMEM;
        }
        size_t name_len = strlen(name) + 1;
        char *name_copy = kmalloc(name_len);
        if (!name_copy) {
            kfree(bits);
            kfree(map);
            kfree(badmap);
            return -ENOMEM;
        }
        memcpy(name_copy, name, name_len);
        memset(map, 0, (size_t)pages);
        size_t available_pages = 0;
        for (uint64_t offset = 0; offset < pages; offset++) {
            if (swap_badmap_test(badmap, offset)) {
                map[offset] = SWAP_MAP_BAD;
            } else {
                /* Bit 0 stays clear: the header is never allocatable. */
                if (offset)
                    swap_bit_set(bits, offset);
                available_pages++;
            }
        }
        flags = spin_lock_irqsave(&swap_locks[type]);
        if (si->active) {
            spin_unlock_irqrestore(&swap_locks[type], flags);
            kfree(map);
            kfree(bits);
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
        swap_free_bits[type] = bits;
        swap_alloc_hint[type] = 1;
        swap_io_refs[type] = 0;
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
    block_dev_t *bdev = si->bdev;
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
    /* Hand the device to the drain path rather than releasing it here.  A
     * swap_page_io() transfer may have snapshotted this same bdev and be inside
     * read_sector()/write_sector() right now; releasing underneath it would
     * drive a device the provider has already torn down. */
    swap_pending_bdev[type] = bdev;
    spin_unlock_irqrestore(&swap_locks[type], flags);

    block_dev_t *drained = swap_take_pending_bdev(type);
    /* Released after the swap lock so the provider's own lock is never
     * nested under it. */
    if (drained && drained->release)
        drained->release(drained);
    kfree(swap_free_bits[type]);
    swap_free_bits[type] = NULL;
    swap_alloc_hint[type] = 1;
    kfree(map);
    kfree((void *)name);
}

int swap_list_area(int type, swap_listing_t *out) {
    if (type < 0 || type >= MAX_SWAPFILES || !out)
        return -EINVAL;
    swap_info_struct *si = &swap_info[type];
    uint64_t flags = spin_lock_irqsave(&swap_locks[type]);
    int rc = -EINVAL;
    if (si->active) {
        out->name[0] = '\0';
        if (si->name) {
            size_t n = 0;
            for (; n + 1 < sizeof(out->name) && si->name[n]; n++)
                out->name[n] = si->name[n];
            out->name[n] = '\0';
        }
        out->pages = si->pages;
        out->inuse_pages = si->inuse_pages;
        rc = 0;
    }
    spin_unlock_irqrestore(&swap_locks[type], flags);
    return rc;
}

swap_entry_t get_swap_page(void) {
    for (unsigned int type = 0; type < MAX_SWAPFILES; type++) {
        swap_info_struct *si = &swap_info[type];
        uint64_t flags = spin_lock_irqsave(&swap_locks[type]);
        if (si->active) {
            uint64_t offset;
            if (swap_bitmap_find_free(si, (int)type, &offset)) {
                /* Clear the bit before publishing the map entry, so a
                 * concurrent swap_free() for this slot cannot see it free. */
                swap_bit_clear(swap_free_bits[type], offset);
                si->swap_map[offset] = 1;
                /* Resume the search past this slot next time. */
                swap_alloc_hint[type] = offset + 1;
                uint64_t stats_flags = spin_lock_irqsave(&swap_stats_lock);
                nr_swap_pages--;
                si->inuse_pages++;
                spin_unlock_irqrestore(&swap_stats_lock, stats_flags);
                spin_unlock_irqrestore(&swap_locks[type], flags);
                return swp_entry(type, offset);
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
        /* Rewind the hint so this slot is reachable again without a full wrap
         * when the area is nearly full. */
        if (swap_alloc_hint[type] > offset)
            swap_alloc_hint[type] = offset;
        swap_bit_set(swap_free_bits[type], offset);
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
    /* Count this transfer against the area.  swap_unregister_device() defers
     * the device release until the count drains, so the bdev pointer above
     * stays live for the whole call below. */
    if (valid)
        swap_io_refs[type]++;
    spin_unlock_irqrestore(&swap_locks[type], flags);
    if (!valid)
        return -EINVAL;

    uint64_t lba = offset * SWAP_SECTORS_PER_PAGE;
    int rc;
    if (write)
        rc = bdev->write_sector(bdev, lba, page, SWAP_SECTORS_PER_PAGE);
    else
        rc = bdev->read_sector(bdev, lba, page, SWAP_SECTORS_PER_PAGE);

    flags = spin_lock_irqsave(&swap_locks[type]);
    if (--swap_io_refs[type] == 0) {
        spin_unlock_irqrestore(&swap_locks[type], flags);
        /* Last one out releases the device if swapoff claimed it meanwhile. */
        block_dev_t *drained = swap_take_pending_bdev(type);
        if (drained && drained->release)
            drained->release(drained);
    } else {
        spin_unlock_irqrestore(&swap_locks[type], flags);
    }
    return rc;
}

int swap_read_page(swap_entry_t entry, void *page) {
    return swap_page_io(entry, page, 0);
}

int swap_write_page(swap_entry_t entry, const void *page) {
    return swap_page_io(entry, (void *)page, 1);
}

#endif
