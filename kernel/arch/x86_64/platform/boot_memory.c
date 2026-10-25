#include "boot_memory.h"
#include "core/errno.h"
#include "core/string.h"

#define BOOT_MAP_LIMIT 0x100000000ULL
#define LOW_END 0x80000000ULL
#define HIGH_BASE 0x100000000ULL
#define HIGH_CHUNK 0x40000000ULL
#define LOW_RESERVED 0x100000ULL
#define MAP_BYTES_MAX 0x10000U
#define MAP_RECORD_MAX 512U
#define RESERVED_MAX 2048U
#define MODULE_MAX 64U
static x86_boot_range_t ram[MAP_RECORD_MAX], reserved[RESERVED_MAX];

static const void *read_physical(x86_boot_memory_reader_t reader, void *context,
                                 uint64_t address, size_t size) {
    if (!size || address >= BOOT_MAP_LIMIT || size > BOOT_MAP_LIMIT - address)
        return NULL;
    return reader(context, address, size);
}
static uint32_t read32(const void *data) {
    uint32_t value;
    memcpy(&value, data, 4);
    return value;
}
static uint64_t read64(const void *data) {
    uint64_t value;
    memcpy(&value, data, 8);
    return value;
}
static int add(x86_boot_range_t *ranges, size_t *count, size_t capacity,
               uint64_t base, uint64_t size) {
    if (!size)
        return 0;
    if (size > (uint64_t)-1 - base)
        return -EOVERFLOW;
    if (*count == capacity)
        return -ENOSPC;
    ranges[(*count)++] = (x86_boot_range_t){base, base + size};
    return 0;
}
static void merge(x86_boot_range_t *ranges, size_t *count) {
    for (size_t i = 1; i < *count; i++) {
        x86_boot_range_t value = ranges[i];
        size_t j = i;
        while (j && ranges[j - 1].base > value.base) {
            ranges[j] = ranges[j - 1];
            j--;
        }
        ranges[j] = value;
    }
    size_t n = 0;
    for (size_t i = 0; i < *count; i++) {
        if (n && ranges[i].base <= ranges[n - 1].end) {
            if (ranges[i].end > ranges[n - 1].end)
                ranges[n - 1].end = ranges[i].end;
        } else
            ranges[n++] = ranges[i];
    }
    *count = n;
}
static int emit(x86_boot_range_t *out, size_t capacity, size_t *count,
                uint64_t base, uint64_t end) {
    if (end <= base)
        return 0;
    if (*count && out[*count - 1].end == base) {
        out[*count - 1].end = end;
        return 0;
    }
    if (*count == capacity)
        return -ENOSPC;
    out[(*count)++] = (x86_boot_range_t){base, end};
    return 0;
}
static int select_range(x86_boot_range_t *out, size_t capacity, size_t *count,
                        uint64_t base, uint64_t end, uint64_t high_end) {
    uint64_t low = base < LOW_RESERVED ? LOW_RESERVED : base,
             stop = end > LOW_END ? LOW_END : end;
    if (low < stop) {
        low = (low + 4095) & ~4095ULL;
        stop &= ~4095ULL;
        int rc = emit(out, capacity, count, low, stop);
        if (rc)
            return rc;
    }
    if (end > high_end)
        end = high_end;
    if (base < HIGH_BASE)
        base = HIGH_BASE;
    if (base >= end)
        return 0;
    if (base > (uint64_t)-1 - (HIGH_CHUNK - 1))
        return -EOVERFLOW;
    base = (base + HIGH_CHUNK - 1) & ~(HIGH_CHUNK - 1);
    end &= ~(HIGH_CHUNK - 1);
    return emit(out, capacity, count, base, end);
}
static int reserve_string(x86_boot_memory_reader_t reader, void *context,
                          uint32_t address, size_t *count) {
    if (!address)
        return 0;
    for (size_t length = 0; length < 4096; length++) {
        const uint8_t *byte =
            read_physical(reader, context, (uint64_t)address + length, 1);
        if (!byte)
            return -EFAULT;
        if (!*byte)
            return add(reserved, count, RESERVED_MAX, address, length + 1);
    }
    return -ENAMETOOLONG;
}
int x86_boot_memory_decode(uint32_t magic, uint32_t info_pa,
                           x86_boot_memory_reader_t reader, void *context,
                           uint64_t fallback_end, uint64_t high_end,
                           const x86_boot_range_t *exclusions,
                           size_t exclusion_count, x86_boot_range_t *out,
                           size_t capacity, size_t *count,
                           enum x86_boot_memory_source *source) {
    if (!reader || !count || !source || (!out && capacity) ||
        (!exclusions && exclusion_count) || fallback_end <= LOW_RESERVED ||
        high_end < HIGH_BASE)
        return -EINVAL;
    *count = 0;
    *source = X86_BOOT_MEMORY_FIXED;
    if (exclusion_count > RESERVED_MAX)
        return -ENOSPC;
    size_t nr = 0, nb = 0;
    int rc;
    for (size_t i = 0; i < exclusion_count; i++) {
        if (exclusions[i].end <= exclusions[i].base)
            return -EINVAL;
        rc = add(reserved, &nb, RESERVED_MAX, exclusions[i].base,
                 exclusions[i].end - exclusions[i].base);
        if (rc)
            return rc;
    }
    struct x86_mb_info info = {0};
    if (magic == X86_MB_BOOTLOADER_MAGIC && !info_pa)
        return -EINVAL;
    if (magic == X86_MB_BOOTLOADER_MAGIC) {
        const void *raw = read_physical(reader, context, info_pa, sizeof(info));
        if (!raw)
            return -EFAULT;
        memcpy(&info, raw, sizeof(info));
        rc = add(reserved, &nb, RESERVED_MAX, info_pa, sizeof(info));
        if (rc)
            return rc;
        if (info.flags & X86_MB_INFO_MEM_MAP) {
            if (!info.mmap_addr || !info.mmap_length)
                return -EINVAL;
            if (info.mmap_length > MAP_BYTES_MAX)
                return -E2BIG;
            const uint8_t *map = read_physical(reader, context, info.mmap_addr,
                                               info.mmap_length);
            if (!map)
                return -EFAULT;
            size_t offset = 0, records = 0;
            while (offset < info.mmap_length) {
                size_t left = info.mmap_length - offset;
                if (left < 4)
                    return -EINVAL;
                uint32_t payload = read32(map + offset);
                if (payload < 20 || payload > left - 4)
                    return -EINVAL;
                if (++records > MAP_RECORD_MAX)
                    return -E2BIG;
                uint64_t base = read64(map + offset + 4),
                         size = read64(map + offset + 12);
                uint32_t type = read32(map + offset + 20);
                rc = type == 1 ? add(ram, &nr, MAP_RECORD_MAX, base, size)
                               : add(reserved, &nb, RESERVED_MAX, base, size);
                if (rc)
                    return rc;
                offset += (size_t)payload + 4;
            }
            rc = add(reserved, &nb, RESERVED_MAX, info.mmap_addr,
                     info.mmap_length);
            if (rc)
                return rc;
            *source = X86_BOOT_MEMORY_MAP;
        } else {
            uint64_t end = fallback_end;
            if (info.flags & X86_MB_INFO_MEMORY) {
                if (!info.mem_upper)
                    return -EINVAL;
                end = LOW_RESERVED + (uint64_t)info.mem_upper * 1024;
                *source = X86_BOOT_MEMORY_BASIC;
            }
            rc =
                add(ram, &nr, MAP_RECORD_MAX, LOW_RESERVED, end - LOW_RESERVED);
            if (rc)
                return rc;
        }
        /* entry.S already snapshots cmdline before clearing BSS; QEMU may
         * place that obsolete source inside BSS. It has no remaining reader. */
        if (info.flags & X86_MB_INFO_MODULES) {
            if (info.mods_count > MODULE_MAX)
                return -E2BIG;
            if (info.mods_count) {
                size_t bytes = (size_t)info.mods_count * 16;
                const uint8_t *mods =
                    read_physical(reader, context, info.mods_addr, bytes);
                if (!info.mods_addr || !mods)
                    return -EFAULT;
                rc = add(reserved, &nb, RESERVED_MAX, info.mods_addr, bytes);
                if (rc)
                    return rc;
                for (uint32_t i = 0; i < info.mods_count; i++) {
                    uint32_t begin = read32(mods + i * 16),
                             end = read32(mods + i * 16 + 4);
                    if (end < begin)
                        return -EINVAL;
                    rc = add(reserved, &nb, RESERVED_MAX, begin,
                             (uint64_t)end - begin);
                    if (rc)
                        return rc;
                    rc = reserve_string(reader, context,
                                        read32(mods + i * 16 + 8), &nb);
                    if (rc)
                        return rc;
                }
            }
        }
        if ((info.flags & (X86_MB_INFO_AOUT | X86_MB_INFO_ELF)) ==
            (X86_MB_INFO_AOUT | X86_MB_INFO_ELF))
            return -EINVAL;
        if (info.flags & (X86_MB_INFO_AOUT | X86_MB_INFO_ELF)) {
            uint64_t bytes = info.flags & X86_MB_INFO_AOUT
                                 ? (uint64_t)info.syms[0] + info.syms[1]
                                 : (uint64_t)info.syms[0] * info.syms[1];
            if (!bytes || !info.syms[2] ||
                bytes > BOOT_MAP_LIMIT - info.syms[2])
                return -EINVAL;
            rc = add(reserved, &nb, RESERVED_MAX, info.syms[2], bytes);
            if (rc)
                return rc;
        }
    } else {
        rc = add(ram, &nr, MAP_RECORD_MAX, LOW_RESERVED,
                 fallback_end - LOW_RESERVED);
        if (rc)
            return rc;
    }
    if (!nr)
        return -ENODEV;
    merge(ram, &nr);
    merge(reserved, &nb);
    for (size_t i = 0; i < nr; i++) {
        uint64_t cursor = ram[i].base, end = ram[i].end;
        for (size_t j = 0; j < nb && cursor < end; j++) {
            if (reserved[j].end <= cursor || reserved[j].base >= end)
                continue;
            if (reserved[j].base > cursor) {
                rc = select_range(out, capacity, count, cursor,
                                  reserved[j].base, high_end);
                if (rc)
                    goto failed;
            }
            cursor = reserved[j].end < end ? reserved[j].end : end;
        }
        rc = select_range(out, capacity, count, cursor, end, high_end);
        if (rc)
            goto failed;
    }
    if (!*count)
        return -ENODEV;
    return 0;
failed:
    *count = 0;
    return rc;
}
