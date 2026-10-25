#ifndef A20_AARCH64_UEFI_HANDOFF_H
#define A20_AARCH64_UEFI_HANDOFF_H
#include <stdint.h>
#include <stddef.h>

/* Private x3 ABI between this EFI image and the VBox AArch64 entry. All
 * addresses are physical. A different version must fail before PFA startup. */
#define A64_UEFI_HANDOFF_MAGIC 0x3149464555303241ULL /* A20UEFI1 */
#define A64_UEFI_HANDOFF_VERSION 1U
#define A64_UEFI_MAP_OFFSET 4096U
#define A64_UEFI_MAP_CAPACITY 65536U
#define A64_UEFI_HANDOFF_BYTES (A64_UEFI_MAP_OFFSET + A64_UEFI_MAP_CAPACITY)
#define A64_UEFI_CMDLINE_OFFSET 256U
#define A64_UEFI_CMDLINE_CAPACITY 1024U
#define A64_UEFI_MAX_DESCRIPTORS 1024U
#define A64_UEFI_RAM_BASE 0x08000000ULL
#define A64_UEFI_RAM_END 0x28000000ULL
#define A64_UEFI_RUNTIME 0x8000000000000000ULL

typedef struct a64_uefi_handoff {
    uint64_t magic;
    uint32_t version, header_bytes;
    uint32_t total_bytes, map_offset;
    uint32_t map_bytes, descriptor_bytes;
    uint32_t descriptor_version, reserved;
    uint64_t kernel_physical, kernel_bytes;
    uint64_t cmdline_physical, cmdline_bytes;
} a64_uefi_handoff_t;

typedef struct a64_uefi_descriptor {
    uint32_t type, padding;
    uint64_t physical, virtual_address, pages, attributes;
} a64_uefi_descriptor_t;
typedef struct a64_uefi_range {
    uint64_t base, size;
} a64_uefi_range_t;
_Static_assert(sizeof(a64_uefi_handoff_t) <= A64_UEFI_CMDLINE_OFFSET,
               "UEFI header/cmdline separation");
_Static_assert(sizeof(a64_uefi_descriptor_t) == 40,
               "UEFI descriptor prefix ABI");

/* Allocation-free parser. Only reclaimable types 1/2/3/4/7 without RUNTIME
 * enter the PFA window. The handoff allocation remains reserved in full. */
int a64_uefi_reserve_range(a64_uefi_range_t *, size_t *count, size_t capacity,
                           uint64_t physical, size_t length);
int a64_uefi_memory_ranges(const void *blob, size_t available,
                           uint64_t physical, uint64_t low, uint64_t high,
                           uint64_t kernel_start, uint64_t kernel_end,
                           a64_uefi_range_t *out, size_t capacity,
                           size_t *count);
#endif
