#ifndef _MM_SWAP_H
#define _MM_SWAP_H

#include <stddef.h>
#include <stdint.h>

#define MAX_SWAPFILES 16
#define SWP_TYPE_BITS 4
#if UINTPTR_MAX == UINT32_MAX
#define SWP_OFFSET_BITS 20
#else
#define SWP_OFFSET_BITS 40
#endif

#define SWAP_MAP_BAD 128
#define SWAP_MAGIC "A20SWAP0"
#define SWAP_VERSION 1

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096UL
#endif

struct swap_header {
    char magic[8];
    uint32_t version;
    uint64_t pages;
    uint32_t bad_pages;
    int32_t priority;
    char label[16];
    uint8_t uuid[16];
    uint8_t reserved[PAGE_SIZE - 60];
    uint8_t badmap[];
} __attribute__((packed));

typedef uint64_t swap_entry_t;

#include "../core/errno.h"

struct block_dev;
typedef struct block_dev block_dev_t;

typedef struct swap_info_struct {
    block_dev_t *bdev;
    const char *name;
    uint8_t *swap_map;
    uint64_t pages;
    size_t inuse_pages;
    int active;
} swap_info_struct;

#if defined(CONFIG_SWAP)
extern swap_info_struct swap_info[MAX_SWAPFILES];
extern size_t total_swap_pages;
extern size_t nr_swap_pages;

int swap_read_page(swap_entry_t entry, void *page);
int swap_write_page(swap_entry_t entry, const void *page);
swap_entry_t get_swap_page(void);
void swap_free(swap_entry_t entry);
void swap_init(void);

int swap_register_device(block_dev_t *bdev, const char *name);
int swap_format_device(block_dev_t *bdev, const char *name, int priority,
                       const char *label);
void swap_unregister_device(int type);
long sys_swapon(const char *path, int flags);
long sys_swapoff(const char *path);
long sys_mkswap(const char *path, int flags);
#else
static inline int swap_read_page(swap_entry_t entry, void *page)
{
    (void)entry;
    (void)page;
    return -1;
}

static inline int swap_write_page(swap_entry_t entry, const void *page)
{
    (void)entry;
    (void)page;
    return -1;
}

static inline swap_entry_t get_swap_page(void)
{
    return 0;
}

static inline void swap_free(swap_entry_t entry)
{
    (void)entry;
}

static inline void swap_init(void)
{
}

static inline long sys_swapon(const char *path, int flags)
{
    (void)path;
    (void)flags;
    return -ENOSYS;
}

static inline long sys_swapoff(const char *path)
{
    (void)path;
    return -ENOSYS;
}

static inline long sys_mkswap(const char *path, int flags)
{
    (void)path;
    (void)flags;
    return -ENOSYS;
}
#endif

#endif
