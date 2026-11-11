/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "drivers/core/amd_iommu.h"
#include "core/errno.h"

static struct {
    uint32_t config[64], original[64];
    uint64_t control, efr;
    unsigned pci_reads, mmio_reads;
    int fail_offset, mmio_fail_offset, fail_status;
} host;
static acpi_ivrs_hardware_t hardware;
static int pci_read(void *context, uint16_t segment, uint16_t rid,
                    uint16_t offset, uint32_t *out) {
    assert(context == &host && segment == 3 && rid == 0x102);
    assert(!(offset & 3) && offset < 256);
    host.pci_reads++;
    if (offset == host.fail_offset)
        return host.fail_status;
    *out = host.config[offset / 4];
    return 0;
}
static int mmio_read(void *context, uint64_t base, uint32_t offset,
                     uint64_t *out) {
    assert(context == &host && base == hardware.base);
    assert(host.config[(hardware.capability + 4) / 4] & 1);
    assert(offset == 0x18 || offset == 0x30);
    host.mmio_reads++;
    if ((int)offset == host.mmio_fail_offset)
        return host.fail_status;
    *out = offset == 0x18 ? host.control : host.efr;
    return 0;
}
static amd_iommu_probe_ops_t ops = {&host, pci_read, mmio_read};
static void reset(void) {
    memset(&host, 0, sizeof(host));
    host.fail_offset = host.mmio_fail_offset = -1;
    host.fail_status = -EIO;
    hardware = (acpi_ivrs_hardware_t){.type = 0x11,
                                      .segment = 3,
                                      .device_id = 0x102,
                                      .capability = 0x60,
                                      .base = 0xfed80000};
    host.config[0] = 0x14191022;
    host.config[0x08 / 4] = 0x08060001;
    host.config[0x04 / 4] = 1U << 20;
    host.config[0x34 / 4] = 0x40;
    host.config[0x40 / 4] =
        0x6005; /* MSI, then firmware's Secure Device cap. */
    host.config[0x60 / 4] = 0x080b000f; /* EFR, revision 1, type 3, ID 0Fh. */
    host.config[0x64 / 4] = hardware.base | 1;
    host.config[0x6c / 4] = 0xff000080;
    host.config[0x70 / 4] = (48U << 8) | (64U << 15);
    host.control = 0x1001;
}
static amd_iommu_snapshot_t probe(int expected, unsigned mmio_reads) {
    amd_iommu_snapshot_t result;
    memset(&result, 0xa5, sizeof(result));
    memcpy(host.original, host.config, sizeof(host.config));
    assert(amd_iommu_probe_unit(&hardware, &ops, &result) == expected);
    assert(host.mmio_reads == mmio_reads);
    assert(!memcmp(host.config, host.original, sizeof(host.config)));
    if (expected)
        assert(!result.base && !result.pci_id && !result.registers_read);
    return result;
}
static void snapshots(void) {
    reset();
    amd_iommu_snapshot_t r = probe(0, 2);
    assert(r.segment == 3 && r.requester == 0x102 && r.capability == 0x60);
    assert(r.pci_id == 0x14191022 && r.base == hardware.base &&
           r.register_size == 0x4000);
    assert(r.mmio_enabled && r.registers_read && r.range_valid &&
           r.control == host.control);
    assert(r.physical_bits == 48 && r.virtual_bits == 64);
    assert(!r.revision_quirk);
    reset();
    host.config[0x60 / 4] &= ~(31U << 19);
    host.config[0x2c / 4] = 0x11001af4;
    r = probe(0, 2);
    assert(r.revision_quirk);
    reset();
    host.config[0x60 / 4] &= ~(31U << 19);
    host.config[0x2c / 4] = 0x11011af4;
    probe(-EOPNOTSUPP, 0);
    reset();
    host.config[0x60 / 4] &= ~(31U << 19);
    host.config[0] = 0x14181022;
    host.config[0x2c / 4] = 0x11001af4;
    probe(-EOPNOTSUPP, 0);
    reset();
    host.config[0x64 / 4] &= ~1U;
    r = probe(0, 0);
    assert(!r.mmio_enabled && !r.registers_read && !r.control);
    reset();
    hardware.type = 0x10;
    host.config[0x64 / 4] &= ~1U;
    r = probe(0, 0);
    assert(!r.register_size);
    reset();
    hardware.type = 0x10;
    host.config[0x60 / 4] &= ~(1U << 27);
    r = probe(0, 1);
    assert(!r.efr && r.registers_read);
    reset();
    hardware.efr = host.efr = 1ULL << 9;
    r = probe(0, 2);
    assert(r.register_size == 0x80000);
    reset();
    hardware.type = 0x10;
    host.efr = 1ULL << 9;
    r = probe(0, 2);
    assert(r.register_size == 0x80000);
    reset();
    hardware.base = 0x123400000000ULL;
    host.config[0x64 / 4] = 1;
    host.config[0x68 / 4] = hardware.base >> 32;
    r = probe(0, 2);
    assert(r.base == hardware.base);
    /* Exact offset, even if an earlier capability has the same ID. */
    reset();
    host.config[0x40 / 4] = 0x080b600f;
    probe(0, 2);
    reset();
    host.config[0x60 / 4] |= 1U << 28;
    probe(0, 2);
    reset();
    host.config[0x6c / 4] =
        0x00100000; /* Ignore invalid fields if RngValid=0. */
    r = probe(0, 2);
    assert(!r.range_valid);
    const unsigned pa[] = {40, 48, 52}, va[] = {32, 40, 48, 64};
    for (unsigned p = 0; p < 3; p++)
        for (unsigned v = 0; v < 4; v++) {
            reset();
            host.config[0x70 / 4] = (pa[p] << 8) | (va[v] << 15);
            r = probe(0, 2);
            assert(r.physical_bits == pa[p] && r.virtual_bits == va[v]);
        }
}
static void invalid_capabilities(void) {
    reset();
    host.config[0] = 0;
    probe(-ENODEV, 0);
    reset();
    host.config[0] = ~0U;
    probe(-ENODEV, 0);
    reset();
    host.config[0x08 / 4] = 0x06000001;
    probe(-EOPNOTSUPP, 0);
    reset();
    host.config[0x0c / 4] = 1U << 16;
    probe(-EOPNOTSUPP, 0);
    reset();
    host.config[0x04 / 4] = 0;
    probe(-ENODEV, 0);
    reset();
    host.config[0x34 / 4] = 0;
    probe(-ENODEV, 0);
    reset();
    host.config[0x34 / 4] = 0x3c;
    probe(-EINVAL, 0);
    reset();
    host.config[0x34 / 4] = 0x41;
    probe(-EINVAL, 0);
    reset();
    host.config[0x40 / 4] = 0x4005;
    probe(-EINVAL, 0); /* Self loop. */
    reset();
    host.config[0x60 / 4] |= 0x4000;
    probe(-EINVAL, 0); /* Multi-node cycle. */
    reset();
    host.config[0x60 / 4] |= 0x8100;
    probe(-EINVAL, 0); /* Broken tail after match. */
    reset();
    host.config[0x60 / 4] |= 0x6400;
    probe(-EINVAL, 0); /* Header inside payload. */
    reset();
    host.config[0x40 / 4] = 0x8005;
    probe(-ENODEV, 0); /* Offset not linked. */
    reset();
    host.config[0x60 / 4] ^= 1;
    probe(-EOPNOTSUPP, 0); /* Wrong cap ID. */
    reset();
    host.config[0x60 / 4] ^= 1U << 16;
    probe(-EOPNOTSUPP, 0);
    reset();
    host.config[0x60 / 4] ^= 1U << 19;
    probe(-EOPNOTSUPP, 0);
    reset();
    host.config[0x60 / 4] &= ~(1U << 27);
    probe(-EIO, 0);
    reset();
    hardware.capability = 0xfc;
    host.config[0x40 / 4] = 0xfc05;
    host.config[0xfc / 4] = 0x080b000f;
    probe(-EINVAL, 0); /* Truncated payload. */
    reset();
    hardware.capability = 0xec;
    host.config[0x40 / 4] = 0xec05;
    host.config[0xec / 4] = 0x180b000f;
    probe(-EINVAL, 0); /* CapExt needs 24 bytes. */
}
static void invalid_apertures(void) {
    reset();
    host.config[0x64 / 4] ^= 0x4000;
    probe(-EIO, 0);
    reset();
    host.config[0x68 / 4] = 1;
    probe(-EIO, 0);
    reset();
    host.config[0x6c / 4] = 0x10800080;
    probe(-EINVAL, 0);
    reset();
    host.config[0x70 / 4] = 0;
    probe(-EOPNOTSUPP, 0);
    reset();
    host.efr = 1ULL << 9;
    probe(-EIO, 2); /* Firmware/MMIO PCSup mismatch. */
    reset();
    hardware.base += 0x4000;
    host.config[0x64 / 4] = hardware.base | 1;
    hardware.efr = host.efr = 1ULL << 9;
    probe(-EINVAL, 0);
    reset();
    hardware.type = 0x10;
    hardware.base += 0x4000;
    host.config[0x64 / 4] = hardware.base | 1;
    host.efr = 1ULL << 9;
    probe(-EINVAL, 2);
    reset();
    hardware.base += 0x1000;
    probe(-EINVAL, 0);
    reset();
    hardware.base = 0;
    probe(-EINVAL, 0);
    reset();
    hardware.capability = 0x61;
    probe(-EINVAL, 0);
    reset();
    hardware.type = 0x20;
    probe(-EINVAL, 0);
}
static void failures(void) {
    const int offsets[] = {0,    8,    12,   4,    0x34, 0x40,
                           0x60, 0x64, 0x68, 0x6c, 0x70};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(*offsets); i++) {
        reset();
        host.fail_offset = offsets[i];
        host.fail_status = -ETIMEDOUT;
        probe(-ETIMEDOUT, 0);
    }
    reset();
    host.fail_offset = 0x60;
    host.fail_status = 1;
    probe(-EIO, 0);
    reset();
    host.mmio_fail_offset = 0x18;
    host.fail_status = -ENOMEM;
    probe(-ENOMEM, 1);
    reset();
    host.mmio_fail_offset = 0x30;
    host.fail_status = 1;
    probe(-EIO, 2);
    reset();
    ops.mmio_read64 = NULL;
    probe(-EOPNOTSUPP, 0);
    host.config[0x64 / 4] &= ~1U;
    probe(0, 0); /* A disabled decoder does not need MMIO transport. */
    ops.mmio_read64 = mmio_read;
    amd_iommu_snapshot_t r;
    assert(amd_iommu_probe_unit(NULL, &ops, &r) == -EINVAL && !r.base);
    assert(amd_iommu_probe_unit(&hardware, NULL, &r) == -EINVAL && !r.base);
    assert(amd_iommu_probe_unit(&hardware, &ops, NULL) == -EINVAL);
}
int main(void) {
    snapshots();
    invalid_capabilities();
    invalid_apertures();
    failures();
    puts("AMD-Vi PCI/MMIO read-only probe, exact capabilities, bounds and "
         "errors: PASS");
    return 0;
}
