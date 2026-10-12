#ifndef A20_EXECUTION_H
#define A20_EXECUTION_H
#include <stdint.h>

/* One fixed-width description for Native programs, firmware and work agents.
 * This is image admission metadata, never proof of shared address mappings or
 * permission to restore another kernel's thread context. */
#define A20_EXECUTION_VERSION 1u
#define A20_EXECUTION_BYTES 64u
#define A20_EXEC_LITTLE_ENDIAN 1u
#define A20_EXEC_BIG_ENDIAN 2u
#define A20_EXEC_ABI_X86_64 1u
#define A20_EXEC_ABI_AARCH64 2u
#define A20_EXEC_ABI_RV64_DOUBLE 3u
#define A20_EXEC_ABI_RV32_DOUBLE 4u
#define A20_EXEC_ABI_ARM_HARD 5u
#define A20_EXEC_ABI_THUMB_SOFT 6u
#define A20_EXEC_ABI_LA64_DOUBLE 7u
#define A20_EXEC_ABI_LA32_SOFT 8u
#define A20_EXEC_ABI_PPC64_ELFV2 9u
#define A20_EXEC_THREAD_BASIC 1u
#define A20_EXEC_NATIVE_IMAGE 2u
#define A20_EXEC_HANDLES 4u
#define A20_EXEC_FUTEX 8u
#define A20_EXEC_WIDE_ARGS 16u
#define A20_EXEC_ATOMIC32 32u
#define A20_EXEC_USER_HEAP 64u
#define A20_EXEC_THREAD_CREATE 128u
#define A20_EXEC_FEATURE_MASK 255u
#define A20_EXEC_MEMORY_FLAT 1u
#define A20_EXEC_MEMORY_PRIVATE_VM 2u
#define A20_EXEC_ATOMIC_NATIVE 1u
#define A20_EXEC_ATOMIC_FABRIC_GATE 2u

typedef struct a20_execution_info {
    uint32_t size, version, elf_machine, pointer_bits;
    uint32_t byte_order, calling_abi, native_major, native_minor;
    uint32_t features, memory_model, atomic_model;
    uint32_t reserved[5];
} a20_execution_info_t;

typedef struct a20_execution_requirement {
    uint32_t elf_machine, pointer_bits, byte_order, calling_abi;
    uint32_t native_major, minimum_minor, required_features;
} a20_execution_requirement_t;

static inline int a20_execution_valid(const a20_execution_info_t *i) {
    if (!i || i->size != A20_EXECUTION_BYTES ||
        i->version != A20_EXECUTION_VERSION || !i->elf_machine ||
        (i->pointer_bits != 32 && i->pointer_bits != 64) ||
        (i->byte_order != A20_EXEC_LITTLE_ENDIAN &&
         i->byte_order != A20_EXEC_BIG_ENDIAN) ||
        !i->calling_abi || i->calling_abi > A20_EXEC_ABI_PPC64_ELFV2 ||
        !i->native_major || i->features & ~A20_EXEC_FEATURE_MASK ||
        (i->memory_model != A20_EXEC_MEMORY_FLAT &&
         i->memory_model != A20_EXEC_MEMORY_PRIVATE_VM) ||
        (i->atomic_model != A20_EXEC_ATOMIC_NATIVE &&
         i->atomic_model != A20_EXEC_ATOMIC_FABRIC_GATE))
        return 0;
    static const uint32_t machines[] = {0,  62, 183, 243, 243,
                                        40, 40, 258, 258, 21};
    static const uint32_t widths[] = {0, 64, 64, 64, 32, 32, 32, 64, 32, 64};
    if (i->elf_machine != machines[i->calling_abi] ||
        i->pointer_bits != widths[i->calling_abi] ||
        ((i->features & A20_EXEC_WIDE_ARGS) && i->pointer_bits != 64))
        return 0;
    for (unsigned k = 0; k < 5; k++)
        if (i->reserved[k])
            return 0;
    return 1;
}

/* Returning true is necessary, not sufficient: loaders must still validate
 * ELF segments, ISA extensions, flags, imports, relocations and entry state.
 * Requirements come from the selected image, not from a convenient target. */
static inline int
a20_execution_image_matches(const a20_execution_info_t *i,
                            const a20_execution_requirement_t *r) {
    return a20_execution_valid(i) && r && i->elf_machine == r->elf_machine &&
           i->pointer_bits == r->pointer_bits &&
           i->byte_order == r->byte_order && i->calling_abi == r->calling_abi &&
           i->native_major == r->native_major &&
           i->native_minor >= r->minimum_minor &&
           !(r->required_features & ~i->features) &&
           (i->features & A20_EXEC_NATIVE_IMAGE);
}

/* Wire serialization is explicit LE even on a big-endian host. Do not send a
 * C struct or depend on member aliasing/padding to define this protocol. */
static inline int a20_execution_encode(uint8_t *p, uint32_t capacity,
                                       const a20_execution_info_t *i) {
    if (!p || capacity < A20_EXECUTION_BYTES || !a20_execution_valid(i))
        return -1;
    uint32_t words[16] = {i->size,
                          i->version,
                          i->elf_machine,
                          i->pointer_bits,
                          i->byte_order,
                          i->calling_abi,
                          i->native_major,
                          i->native_minor,
                          i->features,
                          i->memory_model,
                          i->atomic_model,
                          0,
                          0,
                          0,
                          0,
                          0};
    for (unsigned k = 0; k < 16; k++)
        for (unsigned b = 0; b < 4; b++)
            p[4 * k + b] = (uint8_t)(words[k] >> (8 * b));
    return A20_EXECUTION_BYTES;
}

static inline int a20_execution_decode(const uint8_t *p, uint32_t length,
                                       a20_execution_info_t *i) {
    if (!p || !i || length != A20_EXECUTION_BYTES)
        return -1;
    uint32_t w[16];
    for (unsigned k = 0; k < 16; k++)
        w[k] = (uint32_t)p[4 * k] | (uint32_t)p[4 * k + 1] << 8 |
               (uint32_t)p[4 * k + 2] << 16 | (uint32_t)p[4 * k + 3] << 24;
    a20_execution_info_t result = {
        w[0], w[1], w[2], w[3], w[4],  w[5],
        w[6], w[7], w[8], w[9], w[10], {w[11], w[12], w[13], w[14], w[15]}};
    if (!a20_execution_valid(&result))
        return -1;
    *i = result;
    return 0;
}
#endif
