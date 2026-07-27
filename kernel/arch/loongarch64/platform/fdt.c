#ifdef CONFIG_LOONGARCH64

#include "core/consts.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/types.h"
#include "platform.h"

#define FDT_MAGIC       0xd00dfeedU
#define FDT_BEGIN_NODE  1U
#define FDT_END_NODE    2U
#define FDT_PROP        3U
#define FDT_NOP         4U
#define FDT_END         9U
#define LA_RAM_RANGES   4

typedef struct {
    paddr_t base;
    paddr_t end;
} la_ram_range_t;

extern uint64_t __boot_dtb_ptr;

/* Safe fallback for firmware which does not publish a usable FDT. */
static la_ram_range_t ram_ranges[LA_RAM_RANGES] = {
    { 0x00000000UL, 0x10000000UL },
    { PHYS_MEMORY_BASE, PHYS_MEMORY_END },
};
static size_t ram_range_count = 2;

static uint32_t read_be32(const void *ptr)
{
    const uint8_t *p = (const uint8_t *)ptr;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t read_be64(const void *ptr)
{
    const uint8_t *p = (const uint8_t *)ptr;
    return ((uint64_t)read_be32(p) << 32) | read_be32(p + 4);
}

static int add_ram_range(la_ram_range_t *ranges, size_t *count,
                         uint64_t base, uint64_t size)
{
    if (!size || base + size < base || *count >= LA_RAM_RANGES)
        return -1;

    uint64_t start = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t end = (base + size) & ~(PAGE_SIZE - 1);
    if (end <= start)
        return -1;

    ranges[*count].base = (paddr_t)start;
    ranges[*count].end = (paddr_t)end;
    (*count)++;
    return 0;
}

static int validate_and_sort_ranges(la_ram_range_t *ranges, size_t count)
{
    for (size_t i = 1; i < count; i++) {
        la_ram_range_t cur = ranges[i];
        size_t j = i;
        while (j > 0 && ranges[j - 1].base > cur.base) {
            ranges[j] = ranges[j - 1];
            j--;
        }
        ranges[j] = cur;
    }

    for (size_t i = 0; i < count; i++) {
        if (ranges[i].end <= ranges[i].base)
            return -1;
        if (i && ranges[i - 1].end > ranges[i].base)
            return -1;
    }
    return 0;
}

size_t arch_ram_range_count(void)
{
    return ram_range_count;
}

int arch_ram_range(size_t idx, paddr_t *base, paddr_t *end)
{
    if (!base || !end || idx >= ram_range_count)
        return -1;
    *base = ram_ranges[idx].base;
    *end = ram_ranges[idx].end;
    return 0;
}

void loongarch64_memory_init(void)
{
    const uint8_t *fdt = (const uint8_t *)(uintptr_t)__boot_dtb_ptr;
    if (!fdt || read_be32(fdt) != FDT_MAGIC) {
        printf("[FDT] LoongArch memory map unavailable, using 512 MiB fallback\n");
        return;
    }

    uint32_t totalsize = read_be32(fdt + 4);
    uint32_t off_struct = read_be32(fdt + 8);
    uint32_t off_strings = read_be32(fdt + 12);
    if (totalsize < 40 || off_struct >= totalsize || off_strings >= totalsize) {
        printf("[FDT] malformed LoongArch FDT, using 512 MiB fallback\n");
        return;
    }

    const uint8_t *p = fdt + off_struct;
    const uint8_t *endp = fdt + totalsize;
    const uint8_t *strings = fdt + off_strings;
    la_ram_range_t discovered[LA_RAM_RANGES];
    size_t discovered_count = 0;
    int depth = 0;
    int memory_depth = -1;

    while (p + 4 <= endp) {
        uint32_t token = read_be32(p);
        p += 4;
        if (token == FDT_END)
            break;
        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            const char *name_end = name;
            while ((const uint8_t *)name_end < endp && *name_end)
                name_end++;
            if ((const uint8_t *)name_end >= endp)
                break;
            depth++;
            if (depth == 2 &&
                (strcmp(name, "memory") == 0 ||
                 strncmp(name, "memory@", 7) == 0))
                memory_depth = depth;
            p = (const uint8_t *)(((uintptr_t)(name_end + 1) + 3) & ~3UL);
            continue;
        }
        if (token == FDT_END_NODE) {
            if (depth == memory_depth)
                memory_depth = -1;
            depth--;
            continue;
        }
        if (token == FDT_NOP)
            continue;
        if (token != FDT_PROP || p + 8 > endp)
            break;

        uint32_t len = read_be32(p);
        uint32_t nameoff = read_be32(p + 4);
        p += 8;
        uint32_t padded = (len + 3U) & ~3U;
        if (p + padded > endp || off_strings + nameoff >= totalsize)
            break;

        const char *propname = (const char *)(strings + nameoff);
        if (depth == memory_depth && strcmp(propname, "reg") == 0) {
            for (uint32_t off = 0; off + 16 <= len; off += 16) {
                if (add_ram_range(discovered, &discovered_count,
                                  read_be64(p + off),
                                  read_be64(p + off + 8)) < 0) {
                    discovered_count = 0;
                    break;
                }
            }
        }
        p += padded;
    }

    if (!discovered_count ||
        validate_and_sort_ranges(discovered, discovered_count) < 0) {
        printf("[FDT] invalid LoongArch RAM ranges, using 512 MiB fallback\n");
        return;
    }

    memcpy(ram_ranges, discovered,
           discovered_count * sizeof(discovered[0]));
    ram_range_count = discovered_count;

    uint64_t total = 0;
    for (size_t i = 0; i < ram_range_count; i++) {
        total += ram_ranges[i].end - ram_ranges[i].base;
        printf("[FDT] RAM range %lu: 0x%lx..0x%lx (%lu MiB)\n",
               (unsigned long)i,
               (unsigned long)ram_ranges[i].base,
               (unsigned long)ram_ranges[i].end,
               (unsigned long)((ram_ranges[i].end -
                                ram_ranges[i].base) >> 20));
    }
    printf("[FDT] LoongArch total RAM: %lu MiB\n",
           (unsigned long)(total >> 20));
}

#endif
