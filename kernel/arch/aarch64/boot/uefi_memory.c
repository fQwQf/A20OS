#include "uefi/handoff.h"
#include "core/errno.h"

static int reclaimable(const a64_uefi_descriptor_t *d) {
    return !(d->attributes & A64_UEFI_RUNTIME) &&
           (d->type == 1 || d->type == 2 || d->type == 3 || d->type == 4 ||
            d->type == 7);
}
static int emit(a64_uefi_range_t *out, size_t capacity, size_t *count,
                uint64_t start, uint64_t end) {
    if (start >= end)
        return 0;
    if (*count && out[*count - 1].base + out[*count - 1].size == start) {
        out[*count - 1].size = end - out[*count - 1].base;
        return 0;
    }
    if (*count >= capacity)
        return -ENOSPC;
    out[(*count)++] = (a64_uefi_range_t){start, end - start};
    return 0;
}
int a64_uefi_memory_ranges(const void *blob, size_t available,
                           uint64_t physical, uint64_t low, uint64_t high,
                           uint64_t kernel_start, uint64_t kernel_end,
                           a64_uefi_range_t *out, size_t capacity,
                           size_t *count) {
    if (!blob || !out || !count || ((uintptr_t)blob & 7) || (physical & 4095) ||
        available < sizeof(a64_uefi_handoff_t) || low >= high ||
        physical < low || physical >= high ||
        A64_UEFI_HANDOFF_BYTES > high - physical)
        return -EINVAL;
    *count = 0;
    const a64_uefi_handoff_t *h = blob;
    if (h->magic != A64_UEFI_HANDOFF_MAGIC ||
        h->version != A64_UEFI_HANDOFF_VERSION ||
        h->header_bytes != sizeof(*h) ||
        h->total_bytes != A64_UEFI_HANDOFF_BYTES ||
        h->total_bytes > available || h->map_offset != A64_UEFI_MAP_OFFSET ||
        h->reserved || h->descriptor_version != 1 ||
        h->descriptor_bytes < sizeof(a64_uefi_descriptor_t) ||
        h->descriptor_bytes > 256 || (h->descriptor_bytes & 7) ||
        !h->map_bytes || h->map_bytes > A64_UEFI_MAP_CAPACITY ||
        h->map_bytes % h->descriptor_bytes ||
        h->map_bytes / h->descriptor_bytes > A64_UEFI_MAX_DESCRIPTORS ||
        h->kernel_physical != kernel_start || kernel_start < low ||
        kernel_start >= high || (kernel_start & 4095) ||
        kernel_end <= kernel_start ||
        h->kernel_bytes < kernel_end - kernel_start ||
        h->kernel_bytes > high - kernel_start ||
        ((h->cmdline_physical == 0) != (h->cmdline_bytes == 0)))
        return -EINVAL;
    if (h->cmdline_physical &&
        (h->cmdline_physical != physical + A64_UEFI_CMDLINE_OFFSET ||
         h->cmdline_bytes > A64_UEFI_CMDLINE_CAPACITY ||
         ((const uint8_t *)
              blob)[A64_UEFI_CMDLINE_OFFSET + h->cmdline_bytes - 1] != 0))
        return -EINVAL;
    if (physical < kernel_start + h->kernel_bytes &&
        kernel_start < physical + h->total_bytes)
        return -EINVAL;

    /* Validate the entire descriptor set, including excluded firmware ranges.
     * Sorting indices accepts legal unordered maps without changing the copy. */
    uint16_t order[A64_UEFI_MAX_DESCRIPTORS];
    size_t n = h->map_bytes / h->descriptor_bytes;
    const uint8_t *map = (const uint8_t *)blob + h->map_offset;
    for (size_t i = 0; i < n; i++) {
        const a64_uefi_descriptor_t *d =
            (const void *)(map + i * h->descriptor_bytes);
        if ((d->physical & 4095) || !d->pages ||
            d->pages > ((~(uint64_t)0) - d->physical) / 4096)
            return -EINVAL;
        size_t j = i;
        while (j) {
            const a64_uefi_descriptor_t *previous =
                (const void *)(map + order[j - 1] * h->descriptor_bytes);
            if (previous->physical <= d->physical)
                break;
            order[j] = order[j - 1];
            j--;
        }
        order[j] = (uint16_t)i;
    }
    uint64_t previous_end = 0, kernel_covered = kernel_start,
             handoff_covered = physical;
    for (size_t i = 0; i < n; i++) {
        const a64_uefi_descriptor_t *d =
            (const void *)(map + order[i] * h->descriptor_bytes);
        uint64_t end = d->physical + d->pages * 4096;
        if (i && d->physical < previous_end)
            return -EINVAL;
        previous_end = end;
        if (!reclaimable(d))
            continue;
        if (d->type == 2 && d->physical <= kernel_covered &&
            kernel_covered < end)
            kernel_covered = end;
        if (d->type == 2 && d->physical <= handoff_covered &&
            handoff_covered < end)
            handoff_covered = end;
    }
    if (kernel_covered < kernel_start + h->kernel_bytes ||
        handoff_covered < physical + h->total_bytes)
        return -EINVAL;
    for (size_t i = 0; i < n; i++) {
        const a64_uefi_descriptor_t *d =
            (const void *)(map + order[i] * h->descriptor_bytes);
        if (!reclaimable(d))
            continue;
        uint64_t start = d->physical, end = start + d->pages * 4096;
        /* Reserve the low prefix too; PFA metadata placement reserves the
         * kernel image in the range containing kernel_end. */
        if (start < kernel_start)
            start = kernel_start;
        if (end > high)
            end = high;
        if (start >= end)
            continue;
        int rc;
        if (start < physical && end > physical) {
            rc = emit(out, capacity, count, start, physical);
            if (rc)
                return rc;
            start = physical;
        }
        if (start >= physical && start < physical + h->total_bytes)
            start = physical + h->total_bytes;
        rc = emit(out, capacity, count, start, end);
        if (rc)
            return rc;
    }
    return *count ? 0 : -EINVAL;
}

int a64_uefi_reserve_range(a64_uefi_range_t *ranges, size_t *count,
                           size_t capacity, uint64_t physical, size_t length) {
    if (!ranges || !count || *count > capacity || !length ||
        length > (~(uint64_t)0) - physical ||
        physical + length > (~(uint64_t)0) - 4095)
        return -EINVAL;
    uint64_t low = physical & ~4095ULL,
             high = (physical + length + 4095) & ~4095ULL;
    for (size_t i = 0; i < *count; i++) {
        uint64_t start = ranges[i].base, end = start + ranges[i].size;
        if (low >= end || high <= start)
            continue;
        if (low <= start && high >= end) {
            for (size_t j = i + 1; j < *count; j++)
                ranges[j - 1] = ranges[j];
            (*count)--;
            i--;
            continue;
        }
        if (low <= start) {
            ranges[i] = (a64_uefi_range_t){high, end - high};
            continue;
        }
        if (high >= end) {
            ranges[i].size = low - start;
            continue;
        }
        if (*count >= capacity)
            return -ENOSPC;
        for (size_t j = *count; j > i + 1; j--)
            ranges[j] = ranges[j - 1];
        ranges[i].size = low - start;
        ranges[i + 1] = (a64_uefi_range_t){high, end - high};
        (*count)++;
        i++;
    }
    return *count ? 0 : -EINVAL;
}
