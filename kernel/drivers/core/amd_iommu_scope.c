/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "drivers/core/amd_iommu.h"
#include "drivers/bus/pci_bus.h"
#include "core/errno.h"

/* Only the capability header/PCIe flags are needed. Still validate the tail
 * and uniqueness rather than trusting the first matching header in a cycle. */
static int root_port(device_t *device) {
    if (!(pci_cfg_read32(device, 4) & (1U << 20)))
        return -EOPNOTSUPP;
    unsigned offset = pci_cfg_read32(device, 0x34) & 0xff;
    uint64_t seen = 0;
    uint32_t express = 0;
    unsigned express_offset = 0;
    bool found = false;
    while (offset) {
        if (offset < 0x40 || offset > 0xfc || (offset & 3))
            return -EINVAL;
        uint64_t bit = 1ULL << ((offset - 0x40) / 4);
        if (seen & bit)
            return -EINVAL;
        seen |= bit;
        uint32_t header = pci_cfg_read32(device, offset);
        if ((header & 0xff) == 0x10) {
            if (found)
                return -EINVAL;
            express = header;
            express_offset = offset;
            found = true;
        }
        offset = (header >> 8) & 0xff;
    }
    unsigned version = (express >> 16) & 0xf;
    if (!found || (version != 1 && version != 2) ||
        ((express >> 20) & 0xf) != 4)
        return -EOPNOTSUPP;
    /* Root Status is present in v1; v2 also has Link Status 2 and, when
     * Slot Implemented is set, Slot Status 2. Reject a truncated structure or
     * another list header inside this known mandatory payload. */
    unsigned size = version == 1 ? 0x24 : (express & (1U << 24)) ? 0x3c : 0x34;
    if (express_offset > 256 - size)
        return -EINVAL;
    for (unsigned at = express_offset + 4; at < express_offset + size; at += 4)
        if (seen & (1ULL << ((at - 0x40) / 4)))
            return -EINVAL;
    return 0;
}

int amd_iommu_scope_get(const amd_iommu_snapshot_t *snapshot, device_t *client,
                        device_t **out) {
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!snapshot || !client)
        return -EINVAL;
    if (snapshot->segment)
        return -EOPNOTSUPP;
    /* RngValid=0 explicitly requires IVRS topology (AMD 48882, offset 0Ch).
     * This helper only supplies the supplemental hardware-range evidence. */
    if (!(snapshot->range & 0x80))
        return -EOPNOTSUPP;
    uint8_t first = snapshot->range >> 16, last = snapshot->range >> 24;
    int ret;
    driver_discovery_begin();
    device_t *anchor = NULL;
    ret = pci_device_scope_get(client, (snapshot->range >> 8) & 0xff, first,
                               last, &anchor);
    if (!ret && anchor != client)
        ret = root_port(anchor);
    if (!ret)
        *out = anchor;
    else
        device_put(anchor);
    driver_discovery_end();
    return ret;
}
#endif
