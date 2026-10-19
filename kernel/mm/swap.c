#include "mm/swap.h"

#ifdef CONFIG_SWAP

#include "core/consts.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/random.h"
#include "core/string.h"
#include "drivers/block/block_dev.h"
#include "mm/slab.h"
#include "cg/cgroup.h"

#define SWAP_SECTOR_SIZE 512UL
#define SWAP_SECTORS_PER_PAGE (PAGE_SIZE / SWAP_SECTOR_SIZE)
#define SWP_TYPE_MASK ((1ULL << SWP_TYPE_BITS) - 1)
#define SWP_OFFSET_MASK ((1ULL << SWP_OFFSET_BITS) - 1)

swap_info_struct swap_info[MAX_SWAPFILES];
size_t total_swap_pages;
size_t nr_swap_pages;
static spinlock_t swap_locks[MAX_SWAPFILES];
static spinlock_t swap_stats_lock;
static mutex_t swap_registry_lock; /* swapon/format/swapoff provider ownership */
static struct cg_node **swap_owners[MAX_SWAPFILES];

/*
 * Allocation index, one word per 64 slots of an area's swap_map, with a bit set
 * for every slot that is free and not bad.  swap_map stores a reference count per page,
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
 * In-flight transfers pin the provider without holding the area lock across
 * blocking I/O. swapoff refuses the area until both slot and I/O refs drain.
 */
static unsigned int  swap_io_refs[MAX_SWAPFILES];


static inline void swap_bit_set(uint64_t *bits, uint64_t i)
{
    bits[i >> 6] |= (uint64_t)1 << (i & 63);
}

static size_t swap_bitmap_words(uint64_t pages)
{
    return (size_t)((pages + 63) / 64);
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
    mutex_init(&swap_registry_lock);
    for (int i = 0; i < MAX_SWAPFILES; i++) {
        spin_init(&swap_locks[i]);
        swap_free_bits[i] = NULL;
        swap_owners[i] = NULL;

        swap_alloc_hint[i] = 1;
        swap_io_refs[i] = 0;
    }
}

static int swap_register_device_impl(block_dev_t *bdev, const char *name) {
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

        if (pages > SIZE_MAX / sizeof(uint32_t)) { kfree(badmap); return -ENOMEM; }
        uint32_t *map = kcalloc((size_t)pages, sizeof(*map));
        if (!map) {
            kfree(badmap);
            return -ENOMEM;
        }
        uint64_t *bits = kcalloc(swap_bitmap_words(pages), sizeof(*bits));
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
        struct cg_node **owners = kcalloc((size_t)pages, sizeof(*owners));
        if (!owners) {
            kfree(name_copy); kfree(bits); kfree(map); kfree(badmap);
            return -ENOMEM;
        }
        memcpy(name_copy, name, name_len);

        size_t available_pages = 0;
        for (uint64_t offset = 0; offset < pages; offset++) {
            if (offset < header_pages || swap_badmap_test(badmap, offset)) {
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
            kfree(owners);
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
        swap_owners[type] = owners;
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

int swap_register_device(block_dev_t *bdev, const char *name)
{
    mutex_lock(&swap_registry_lock);
    int r = swap_register_device_impl(bdev, name);
    mutex_unlock(&swap_registry_lock);
    return r;
}

static int swap_format_device_impl(block_dev_t *bdev, const char *name, int priority,
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

int swap_format_device(block_dev_t *bdev, const char *name, int priority,
                        const char *label)
{
    mutex_lock(&swap_registry_lock);
    int r = 0;
    for (int i = 0; i < MAX_SWAPFILES; i++) {
        uint64_t flags = spin_lock_irqsave(&swap_locks[i]);
        int busy = swap_info[i].active && swap_info[i].bdev == bdev;
        spin_unlock_irqrestore(&swap_locks[i], flags);
        if (busy) { r = -EBUSY; break; }
    }
    if (!r) r = swap_format_device_impl(bdev, name, priority, label);
    mutex_unlock(&swap_registry_lock);
    return r;
}

static int swap_unregister_device_impl(int type, const char *expected_name) {
    if (type < 0 || type >= MAX_SWAPFILES)
        return -EINVAL;

    swap_info_struct *si = &swap_info[type];
    uint64_t flags = spin_lock_irqsave(&swap_locks[type]);
    uint32_t *map = si->swap_map;
    const char *name = si->name;
    block_dev_t *bdev = si->bdev;
    if (!si->active || (expected_name && strcmp(si->name, expected_name))) {
        spin_unlock_irqrestore(&swap_locks[type], flags);
        return -ENOENT;
    }
    /* Check under the same lock that allocates and duplicates slot refs. */
    if (si->inuse_pages || swap_io_refs[type]) {
        spin_unlock_irqrestore(&swap_locks[type], flags);
        return -EBUSY;
    }
    uint64_t *bits = swap_free_bits[type];
    struct cg_node **owners = swap_owners[type];
    swap_owners[type] = NULL;
    swap_free_bits[type] = NULL;
    swap_alloc_hint[type] = 1;
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
    /* Live slots and I/O were rejected under the area lock. The detached
     * provider is ours alone even if this area index is immediately reused. */
    if (bdev && bdev->release) bdev->release(bdev);
    kfree(bits);
    kfree(owners);
    kfree(map);
    kfree((void *)name);
    return 0;
}

int swap_unregister_device(int type, const char *expected_name)
{
    mutex_lock(&swap_registry_lock);
    int r = swap_unregister_device_impl(type, expected_name);
    mutex_unlock(&swap_registry_lock);
    return r;
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

/*
 * Slot allocation, its reference counting and the release path live in
 * kernel/mm/swap_slots.rs.  What is left here is the area lock, the area and
 * registry state, the counter lock, the I/O path and the cgroup charge/pin.
 */
extern swap_entry_t get_swap_page_owned(struct cg_node *owner);
extern swap_entry_t get_swap_page(void);
extern int swap_duplicate(swap_entry_t entry);
extern void swap_free(swap_entry_t entry);

struct a20_swap_area {
    void     *map;   /* uint32_t *swap_map */
    void     *bits;  /* uint64_t *swap_free_bits */
    uint64_t  pages;
    uint64_t  hint;
    int32_t   active;
    int32_t   reserved;
};
#if __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(struct a20_swap_area) == 40, "Rust/C swap area ABI");
#endif

uint64_t a20_swap_lock(unsigned int type)
{
    return spin_lock_irqsave(&swap_locks[type]);
}

void a20_swap_unlock(unsigned int type, uint64_t flags)
{
    spin_unlock_irqrestore(&swap_locks[type], flags);
}

void a20_swap_area(unsigned int type, struct a20_swap_area *out)
{
    swap_info_struct *si = &swap_info[type];
    if (!out)
        return;
    out->map = si->swap_map;
    out->bits = swap_free_bits[type];
    out->pages = si->pages;
    out->hint = swap_alloc_hint[type];
    out->active = si->active;
    out->reserved = 0;
}

void a20_swap_set_hint(unsigned int type, uint64_t hint)
{
    swap_alloc_hint[type] = hint;
}

void *a20_swap_owner(unsigned int type, uint64_t offset)
{
    if (!swap_owners[type] || offset >= swap_info[type].pages)
        return NULL;
    return swap_owners[type][offset];
}

void a20_swap_set_owner(unsigned int type, uint64_t offset, void *owner)
{
    if (!swap_owners[type] || offset >= swap_info[type].pages)
        return;
    swap_owners[type][offset] = owner;
}

/* The two counters move together under their own lock, so a concurrent
 * swapoff sees either both or neither. */
void a20_swap_account(unsigned int type, int alloc)
{
    uint64_t flags = spin_lock_irqsave(&swap_stats_lock);
    if (alloc) {
        nr_swap_pages--;
        swap_info[type].inuse_pages++;
    } else {
        swap_info[type].inuse_pages--;
        nr_swap_pages++;
    }
    spin_unlock_irqrestore(&swap_stats_lock, flags);
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
                offset > 0 && si->swap_map[offset] &&
                si->swap_map[offset] != SWAP_MAP_BAD;
    /* swapoff rejects a nonzero I/O count, keeping this provider live. */
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
    --swap_io_refs[type];
    spin_unlock_irqrestore(&swap_locks[type], flags);
    return rc;
}

int swap_read_page(swap_entry_t entry, void *page) {
    return swap_page_io(entry, page, 0);
}

int swap_write_page(swap_entry_t entry, const void *page) {
    return swap_page_io(entry, (void *)page, 1);
}

#endif
