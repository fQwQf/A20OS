#include "boot/framebuffer.h"
#include "core/arch.h"
#include "core/errno.h"
extern uint32_t boot_pgdir[];
extern void *arch_device_map_mmio(uint64_t, size_t, uint32_t);

int arch_boot_framebuffer_reservation(const boot_framebuffer_t *fb,
                                      uint64_t *base, uint64_t *bytes) {
    uint64_t end = fb->physical + fb->aperture_bytes;
    if (end < fb->physical || end > (uint64_t)-1 - 0xfffff)
        return -EOVERFLOW;
    *base = fb->physical & ~0xfffffULL;
    *bytes = ((end + 0xfffff) & ~0xfffffULL) - *base;
    return 0;
}
#ifndef CONFIG_NOMMU
#define SECTION_SHAREABLE (1U << 16)
static uint32_t saved[4096];
static uint32_t *root(void) {
    return (uint32_t *)((uintptr_t)boot_pgdir + PAGE_OFFSET);
}
static int supported_cache(void) {
    uint32_t midr;
    __asm__ __volatile__("mrc p15, 0, %0, c0, c0, 0" : "=r"(midr));
    /* Cortex-A7 (V3s) and Cortex-A15 (QEMU virt) have PIPT data caches.
     * An unknown VIPT aliasing implementation needs its own cache contract. */
    unsigned part = (midr >> 4) & 0xfff;
    return (midr >> 24) == 0x41 && (part == 0xc07 || part == 0xc0f);
}
static void clean(const void *alias, size_t bytes) {
    uint32_t ctr;
    __asm__ __volatile__("mrc p15, 0, %0, c0, c0, 1" : "=r"(ctr));
    size_t line = 4U << ((ctr >> 16) & 15);
    uintptr_t first = (uintptr_t)alias & ~((uintptr_t)line - 1);
    uintptr_t end = (uintptr_t)alias + bytes;
    for (uintptr_t at = first; at < end; at += line)
        __asm__ __volatile__("mcr p15, 0, %0, c7, c10, 1" ::"r"(at) : "memory");
    __asm__ __volatile__("dsb sy" ::: "memory");
}
static int is_wb_section(uint32_t descriptor) {
    return (descriptor & 3) == 2 && (descriptor & 0x700c) == 0xc;
}
static void *ram_alias(const boot_framebuffer_t *fb) {
    if (!supported_cache() || fb->physical >= 0x80000000ULL ||
        fb->aperture_bytes > 0x80000000ULL - fb->physical)
        return NULL;
    uint64_t end = fb->physical + fb->aperture_bytes;
    for (uint64_t at = fb->physical; at < end;
         at = (at & ~0xfffffULL) + 0x100000) {
        uint32_t d = root()[(uint32_t)(at + PAGE_OFFSET) >> 20];
        if (!is_wb_section(d) || !(d & SECTION_SHAREABLE) ||
            (d & 0xfff00000U) != (at & 0xfff00000ULL))
            return NULL;
    }
    return (void *)(uintptr_t)(fb->physical + PAGE_OFFSET);
}
static int overlaps(uint64_t first, uint64_t bytes, uint64_t base,
                    uint64_t end) {
    return first < end && base < first + bytes;
}
#endif
int arch_boot_framebuffer_prepare(const boot_framebuffer_t *fb,
                                  enum boot_framebuffer_memory *memory,
                                  uint64_t dtb, size_t dtb_bytes) {
    if (boot_framebuffer_validate(fb))
        return -EINVAL;
#ifndef CONFIG_NOMMU
    if (*memory == BOOT_FRAMEBUFFER_FIRMWARE) {
        int ram = fb->physical >= PHYS_MEMORY_BASE &&
                  fb->physical < PHYS_MEMORY_END &&
                  fb->aperture_bytes <= PHYS_MEMORY_END - fb->physical;
        if (ram)
            for (uint64_t at = fb->physical;
                 at < fb->physical + fb->aperture_bytes;
                 at = (at & ~0xfffffULL) + 0x100000) {
                uint32_t d = root()[(uint32_t)(at + PAGE_OFFSET) >> 20];
                if (!is_wb_section(d) ||
                    (d & 0xfff00000U) != (at & 0xfff00000ULL))
                    ram = 0;
            }
        *memory = ram ? BOOT_FRAMEBUFFER_RAM : BOOT_FRAMEBUFFER_DEVICE;
    }
#endif
    if (*memory == BOOT_FRAMEBUFFER_DEVICE)
        return arch_device_map_mmio(fb->physical, (size_t)fb->aperture_bytes, 0)
                   ? 0
                   : -EOPNOTSUPP;
    if (*memory != BOOT_FRAMEBUFFER_RAM)
        return -EINVAL;
#ifdef CONFIG_NOMMU
    (void)dtb;
    (void)dtb_bytes;
    /* No user/kernel cache-attribute translation exists on this profile. */
    return -EOPNOTSUPP;
#else
    if (!supported_cache())
        return -EOPNOTSUPP;
    uint64_t first, bytes;
    int rc = arch_boot_framebuffer_reservation(fb, &first, &bytes);
    extern char _end[];
    uint64_t kernel_end = (uintptr_t)_end - PAGE_OFFSET;
    if (rc || first >= 0x80000000ULL || bytes > 0x80000000ULL - first ||
        overlaps(first, bytes, PHYS_MEMORY_BASE, kernel_end) ||
        (dtb_bytes && overlaps(first, bytes, dtb, dtb + dtb_bytes)))
        return -EINVAL;
    uint32_t *table = root();
    unsigned aliases = 0;
    for (uint64_t at = fb->physical; at < fb->physical + fb->aperture_bytes;
         at = (at & ~0xfffffULL) + 0x100000) {
        uint32_t d = table[(uint32_t)(at + PAGE_OFFSET) >> 20];
        if (!is_wb_section(d) || (d & 0xfff00000U) != (at & 0xfff00000ULL))
            return -EOPNOTSUPP;
    }
    /* Preflight every bootstrap alias. Refuse L2 aliases rather than modifying
     * part of a live table or assuming the direct alias is the only one. */
    for (unsigned i = 0; i < 4096; i++) {
        uint32_t d = table[i];
        saved[i] = 0;
        if ((d & 3) == 1)
            return -EOPNOTSUPP;
        else if ((d & 3) == 2 &&
                 overlaps(first, bytes, d & 0xfff00000U,
                          (uint64_t)(d & 0xfff00000U) + 0x100000)) {
            if (!is_wb_section(d) || (d & (1U << 18)))
                return -EOPNOTSUPP;
            saved[i] = d;
            aliases++;
        }
    }
    if (!aliases)
        return -EOPNOTSUPP;
    /* Every touched section is excluded from PFA by the architecture-specific
     * reservation hook. No neighbour can later acquire a mismatched RO alias. */
    for (unsigned i = 0; i < 4096; i++)
        if (saved[i])
            clean((void *)(uintptr_t)(i << 20), 0x100000);
    for (unsigned i = 0; i < 4096; i++)
        if (saved[i])
            table[i] = 0;
    clean(table, 4096 * sizeof(*table));
    arch_tlb_flush();
    for (unsigned i = 0; i < 4096; i++)
        if (saved[i])
            table[i] = saved[i] | SECTION_SHAREABLE;
    clean(table, 4096 * sizeof(*table));
    arch_tlb_flush();
    return ram_alias(fb) ? 0 : -EOPNOTSUPP;
#endif
}
void *arch_boot_framebuffer_map(const boot_framebuffer_t *fb, unsigned memory) {
    if (boot_framebuffer_validate(fb))
        return NULL;
    if (memory == BOOT_FRAMEBUFFER_DEVICE)
        return arch_device_map_mmio(fb->physical, (size_t)fb->aperture_bytes,
                                    0);
#ifndef CONFIG_NOMMU
    if (memory == BOOT_FRAMEBUFFER_RAM)
        return ram_alias(fb);
#endif
    return NULL;
}
int arch_boot_framebuffer_user_flags(const boot_framebuffer_t *fb,
                                     unsigned memory, uintptr_t requested,
                                     uintptr_t *out) {
    if (!arch_boot_framebuffer_map(fb, memory))
        return -EOPNOTSUPP;
    /* PTE_W shares the hardware S bit. Only RW framebuffer mappings can
     * express the same shareable WB type as the reserved kernel sections. */
    if (memory == BOOT_FRAMEBUFFER_RAM && !(requested & PTE_W))
        return -EOPNOTSUPP;
    *out = (requested & ~(PTE_DEVICE | PTE_MAT1)) |
           (memory == BOOT_FRAMEBUFFER_RAM ? PTE_MAT1 : PTE_DEVICE);
    return 0;
}
int arch_boot_framebuffer_flush(const boot_framebuffer_t *fb, unsigned memory) {
    void *alias = arch_boot_framebuffer_map(fb, memory);
    if (!alias)
        return -EOPNOTSUPP;
#ifndef CONFIG_NOMMU
    if (memory == BOOT_FRAMEBUFFER_RAM)
        clean(alias, (size_t)fb->stride * fb->height);
#endif
    __asm__ __volatile__("dsb sy" ::: "memory");
    return 0;
}
