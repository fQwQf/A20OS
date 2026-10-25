#ifndef A20_X86_BOOT_MEMORY_H
#define A20_X86_BOOT_MEMORY_H
#include "core/types.h"

/* Multiboot 1 v0.6.96: size excludes its own four bytes. Physical boot
 * metadata must be covered by an independently established readable window. */
#define X86_MB_BOOTLOADER_MAGIC 0x2badb002U
#define X86_MB_INFO_MEMORY (1U << 0)
#define X86_MB_INFO_CMDLINE (1U << 2)
#define X86_MB_INFO_MODULES (1U << 3)
#define X86_MB_INFO_AOUT (1U << 4)
#define X86_MB_INFO_ELF (1U << 5)
#define X86_MB_INFO_MEM_MAP (1U << 6)
struct x86_mb_info {
    uint32_t flags, mem_lower, mem_upper, boot_device, cmdline;
    uint32_t mods_count, mods_addr, syms[4], mmap_length, mmap_addr;
};
typedef struct x86_boot_range {
    uint64_t base, end;
} x86_boot_range_t;
typedef const void *(*x86_boot_memory_reader_t)(void *context, uint64_t pa,
                                                size_t bytes);
enum x86_boot_memory_source {
    X86_BOOT_MEMORY_FIXED,
    X86_BOOT_MEMORY_BASIC,
    X86_BOOT_MEMORY_MAP
};
/* Allocation-free, boot-CPU only. Rejects malformed present maps, overlapping
 * reserved storage wins over RAM, and capacity failures never truncate.
 * Only the legacy no-map path may use fallback_end. High RAM is returned only
 * in complete usable 1 GiB chunks so the bootstrap can map it safely. */
int x86_boot_memory_decode(uint32_t magic, uint32_t info_pa,
                           x86_boot_memory_reader_t reader, void *context,
                           uint64_t fallback_end, uint64_t high_end,
                           const x86_boot_range_t *exclusions,
                           size_t exclusion_count, x86_boot_range_t *out,
                           size_t capacity, size_t *count,
                           enum x86_boot_memory_source *source);
#endif
