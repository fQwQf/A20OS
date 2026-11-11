/* SPDX-License-Identifier: Apache-2.0 */
#include "drivers/core/amd_iommu.h"
#include "core/errno.h"

static int read_pci(const amd_iommu_probe_ops_t *ops,
                    const acpi_ivrs_hardware_t *h, unsigned offset,
                    uint32_t *out) {
    int ret =
        ops->pci_read32(ops->context, h->segment, h->device_id, offset, out);
    return ret > 0 ? -EIO : ret;
}

/* A matching header near the front does not excuse a broken tail or a cycle.
 * One function can contain several Secure Device capabilities: do not take
 * the first capability with ID 0Fh in place of the exact firmware offset. */
static int capability(const amd_iommu_probe_ops_t *ops,
                      const acpi_ivrs_hardware_t *h, uint32_t pci_id,
                      uint32_t *out, bool *revision_quirk) {
    uint32_t value;
    int ret = read_pci(ops, h, 0x04, &value);
    if (ret)
        return ret;
    if (!(value & (1U << 20)))
        return -ENODEV;
    ret = read_pci(ops, h, 0x34, &value);
    if (ret)
        return ret;
    unsigned offset = value & 0xff;
    uint64_t seen = 0;
    bool found = false;
    while (offset) {
        if (offset < 0x40 || (offset & 3))
            return -EINVAL;
        uint64_t bit = 1ULL << ((offset - 0x40) / 4);
        if (seen & bit)
            return -EINVAL;
        seen |= bit;
        ret = read_pci(ops, h, offset, &value);
        if (ret)
            return ret;
        if (offset == h->capability) {
            *out = value;
            found = true;
        }
        offset = (value >> 8) & 0xff;
    }
    if (!found)
        return -ENODEV;
    if ((*out & 0xff) != 0x0f || ((*out >> 16) & 7) != 3)
        return -EOPNOTSUPP;
    unsigned revision = (*out >> 19) & 31;
    if (revision != 1) {
        /* QEMU 10.0's AMDVI_CAPAB_FEATURES omits CapRev. Keep the exception
         * restricted to its AMD-Vi device AND QEMU subsystem identity; this
         * does not establish revision-0 support for physical IOMMUs. */
        if (revision || pci_id != 0x14191022)
            return -EOPNOTSUPP;
        ret = read_pci(ops, h, 0x2c, &value);
        if (ret)
            return ret;
        if (value != 0x11001af4)
            return -EOPNOTSUPP;
        *revision_quirk = true;
    }
    unsigned size = (*out & (1U << 28)) ? 24 : 20;
    if (h->capability > 256 - size)
        return -EINVAL;
    for (unsigned off = h->capability + 4; off < h->capability + size; off += 4)
        if (seen & (1ULL << ((off - 0x40) / 4)))
            return -EINVAL;
    return 0;
}

int amd_iommu_probe_unit(const acpi_ivrs_hardware_t *h,
                         const amd_iommu_probe_ops_t *ops,
                         amd_iommu_snapshot_t *out) {
    if (!out)
        return -EINVAL;
    *out = (amd_iommu_snapshot_t){0};
    if (!h || !ops || !ops->pci_read32 ||
        (h->type != 0x10 && h->type != 0x11 && h->type != 0x40) || !h->base ||
        (h->base & 0x3fff) || h->base > ~(uint64_t)0 - 0x3fff ||
        h->capability < 0x40 || h->capability > 0xfc || (h->capability & 3))
        return -EINVAL;
    amd_iommu_snapshot_t result = {.segment = h->segment,
                                   .requester = h->device_id,
                                   .capability = h->capability,
                                   .base = h->base};
    int ret = read_pci(ops, h, 0, &result.pci_id);
    if (ret)
        return ret;
    if (!(result.pci_id & 0xffff) || (result.pci_id & 0xffff) == 0xffff)
        return -ENODEV;
    uint32_t value;
    ret = read_pci(ops, h, 0x08, &value);
    if (ret)
        return ret;
    if ((value >> 8) != 0x080600)
        return -EOPNOTSUPP;
    ret = read_pci(ops, h, 0x0c, &value);
    if (ret)
        return ret;
    if (((value >> 16) & 0x7f) != 0)
        return -EOPNOTSUPP;
    ret = capability(ops, h, result.pci_id, &result.capability_header,
                     &result.revision_quirk);
    if (ret)
        return ret;
    if (h->type != 0x10 && !(result.capability_header & (1U << 27)))
        return -EIO;
    uint32_t low, high;
    ret = read_pci(ops, h, h->capability + 4, &low);
    if (ret)
        return ret;
    ret = read_pci(ops, h, h->capability + 8, &high);
    if (ret)
        return ret;
    if ((((uint64_t)high << 32) | (low & ~0x3fffU)) != h->base)
        return -EIO;
    result.mmio_enabled = low & 1;
    ret = read_pci(ops, h, h->capability + 12, &result.range);
    if (ret)
        return ret;
    result.range_valid = result.range & 0x80;
    if (result.range_valid &&
        ((result.range >> 16) & 0xff) > (result.range >> 24))
        return -EINVAL;
    ret = read_pci(ops, h, h->capability + 16, &result.misc);
    if (ret)
        return ret;
    result.physical_bits = (result.misc >> 8) & 0x7f;
    result.virtual_bits = (result.misc >> 15) & 0x7f;
    if ((result.physical_bits != 40 && result.physical_bits != 48 &&
         result.physical_bits != 52) ||
        (result.virtual_bits != 32 && result.virtual_bits != 40 &&
         result.virtual_bits != 48 && result.virtual_bits != 64))
        return -EOPNOTSUPP;
    bool counters = h->type != 0x10 && (h->efr & (1ULL << 9));
    if (counters && ((h->base & 0x7ffff) || h->base > ~(uint64_t)0 - 0x7ffff))
        return -EINVAL;
    if (result.mmio_enabled) {
        if (!ops->mmio_read64)
            return -EOPNOTSUPP;
        ret = ops->mmio_read64(ops->context, h->base, 0x18, &result.control);
        if (ret)
            return ret > 0 ? -EIO : ret;
        if (result.capability_header & (1U << 27)) {
            ret = ops->mmio_read64(ops->context, h->base, 0x30, &result.efr);
            if (ret)
                return ret > 0 ? -EIO : ret;
            bool observed = result.efr & (1ULL << 9);
            if (h->type != 0x10 && observed != counters)
                return -EIO;
            counters = observed;
        }
        result.registers_read = true;
    }
    uint64_t size = counters ? 0x80000 : 0x4000;
    if ((h->base & (size - 1)) || h->base > ~(uint64_t)0 - (size - 1))
        return -EINVAL;
    /* Legacy IVHD has no EFR image. With a disabled decoder and EFR support,
     * only the minimum window is known; do not advertise a final 16-KiB size. */
    if (h->type != 0x10 || result.mmio_enabled ||
        !(result.capability_header & (1U << 27)))
        result.register_size = size;
    *out = result;
    return 0;
}
