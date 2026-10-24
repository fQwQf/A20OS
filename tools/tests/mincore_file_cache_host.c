/* Exercise the production sys_mincore function with page-table/cache stubs. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#define PAGE_SIZE 4096UL
#define ROUND_UP(value, alignment) (((value) + (alignment) - 1) & ~((alignment) - 1))
#define PTE_V 1UL
#define VM_FILE (1UL << 13)
#define VM_VMO (1UL << 14)
#define VM_PFNMAP (1UL << 17)

typedef uintptr_t pte_t;
struct vnode { int identity; };
typedef struct mm_seg {
    uint64_t start;
    uint64_t end;
    uint64_t backing_offset;
    uint64_t vm_flags;
    struct vnode *file_vnode;
} mm_seg_t;
typedef struct mm_struct { pte_t *pgdir; } mm_struct_t;
typedef struct task { mm_struct_t *mm; } task_t;
typedef struct page_cache_page {
    int present;
    int uptodate;
    int pins;
} page_cache_page_t;

static mm_seg_t segment;
static mm_struct_t mm;
static task_t task;
static struct vnode vnode;
static pte_t ptes[8];
static page_cache_page_t cached[64];
static unsigned char scratch[64];
static int mm_locked;
static int lock_count, unlock_count, copy_count, lookup_count;
static int cache_get_count, cache_put_count, create_requests, io_count;
static uint64_t queried_indices[8];

static task_t *proc_current(void)
{
    return &task;
}

static unsigned char *proc_scratch_buffer(size_t bytes)
{
    return bytes <= sizeof(scratch) ? scratch : NULL;
}

static uint64_t linux_mm_lock(task_t *current)
{
    assert(current == &task && !mm_locked);
    mm_locked = 1;
    lock_count++;
    return 0x55;
}

static void linux_mm_unlock(task_t *current, uint64_t flags)
{
    assert(current == &task && mm_locked && flags == 0x55);
    mm_locked = 0;
    unlock_count++;
}

static mm_seg_t *mm_seg_find(mm_struct_t *address_space, uint64_t va)
{
    assert(address_space == &mm && mm_locked);
    lookup_count++;
    return va >= segment.start && va < segment.end ? &segment : NULL;
}

static pte_t *pt_lookup_leaf(pte_t *pgdir, uint64_t va,
                             void *level, void *base, void *size)
{
    (void)level;
    (void)base;
    (void)size;
    assert(pgdir == ptes && mm_locked);
    return &ptes[(va - segment.start) / PAGE_SIZE];
}

static page_cache_page_t *page_cache_get(struct vnode *vn, uint64_t index,
                                         int create)
{
    assert(mm_locked && vn == &vnode && index < 64);
    assert(create == 0);
    queried_indices[cache_get_count] = index;
    cache_get_count++;
    create_requests += create != 0;
    if (!cached[index].present) {
        return NULL;
    }
    cached[index].pins++;
    return &cached[index];
}

static int page_cache_is_uptodate(page_cache_page_t *page)
{
    assert(mm_locked && page);
    return page->uptodate;
}

static void page_cache_put(page_cache_page_t *page)
{
    assert(mm_locked && page && page->pins > 0);
    page->pins--;
    cache_put_count++;
}

static int copy_to_user(unsigned char *destination,
                        const unsigned char *source, size_t bytes)
{
    assert(!mm_locked && destination);
    memcpy(destination, source, bytes);
    copy_count++;
    return 0;
}

/* PRODUCTION */

static void reset(void)
{
    memset(&segment, 0, sizeof(segment));
    memset(&mm, 0, sizeof(mm));
    memset(&task, 0, sizeof(task));
    memset(ptes, 0, sizeof(ptes));
    memset(cached, 0, sizeof(cached));
    memset(scratch, 0, sizeof(scratch));
    memset(queried_indices, 0, sizeof(queried_indices));
    mm.pgdir = ptes;
    task.mm = &mm;
    segment.start = 0x1000;
    segment.end = 0x5000;
    segment.file_vnode = &vnode;
    lock_count = unlock_count = copy_count = lookup_count = 0;
    cache_get_count = cache_put_count = create_requests = io_count = 0;
    mm_locked = 0;
}

static void cache_page(uint64_t index, int uptodate)
{
    cached[index].present = 1;
    cached[index].uptodate = uptodate;
}

int main(void)
{
    unsigned char vec[4] = {0};
    reset();
    segment.vm_flags = VM_FILE;
    segment.backing_offset = 7 * PAGE_SIZE;
    ptes[1] = PTE_V;
    cache_page(7, 1);
    cache_page(9, 1);
    cache_page(10, 0);
    assert(sys_mincore(0x1000, 4 * PAGE_SIZE, vec) == 0);
    assert(vec[0] == 1 && vec[1] == 1 && vec[2] == 1 && vec[3] == 0);
    assert(cache_get_count == 3 && cache_put_count == 3);
    assert(queried_indices[0] == 7 && queried_indices[1] == 9 &&
           queried_indices[2] == 10);
    assert(create_requests == 0 && io_count == 0);
    assert(cached[7].pins == 0 && cached[9].pins == 0 && cached[10].pins == 0);
    assert(lock_count == 1 && unlock_count == 1 && !mm_locked && copy_count == 1);

    /* Anonymous and VMO mappings keep the absent-PTE result nonresident. */
    reset();
    segment.vm_flags = 0;
    memset(vec, 0xff, sizeof(vec));
    assert(sys_mincore(0x1000, 4 * PAGE_SIZE, vec) == 0);
    assert(vec[0] == 0 && vec[1] == 0 && cache_get_count == 0);

    reset();
    segment.vm_flags = VM_FILE | VM_VMO;
    assert(sys_mincore(0x1000, PAGE_SIZE, vec) == 0);
    assert(vec[0] == 0 && cache_get_count == 0);

    /* Invalid ranges retain the previous ENOMEM behavior and unlock cleanly. */
    reset();
    segment.end = segment.start;
    assert(sys_mincore(0x1000, PAGE_SIZE, vec) == -ENOMEM);
    assert(lock_count == 1 && unlock_count == 1 && !mm_locked);

    puts("MINCORE_FILE_CACHE: production sys_mincore host fixture PASS");
    return 0;
}
