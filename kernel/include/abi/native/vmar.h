/*
 * A20OS Native ABI — Virtual Memory Address Region (VMAR).
 * Design reference: docs/native-abi/04-memory.md §3
 */
#ifndef _ABI_NATIVE_VMAR_H
#define _ABI_NATIVE_VMAR_H

#include "core/types.h"
#include "abi/native/vmo.h"

struct a20_vmar {
    uint64_t      base;
    uint64_t      length;
    uint32_t      prot;
    uint32_t      flags;
    struct a20_vmo *vmo;
    uint64_t      vmo_offset;
};

typedef struct a20_vmar a20_vmar_t;

uint64_t a20_vmar_find_free(uint64_t hint, uint64_t length);
int64_t  a20_vmar_map(struct a20_vmo *vmo, uint64_t vmo_offset, uint64_t length,
                      uint32_t prot, uint32_t flags, uint64_t hint, uint64_t *out_addr);
int64_t  a20_vmar_unmap(uint64_t addr, uint64_t length);
int64_t  a20_vmar_protect(uint64_t addr, uint64_t length, uint32_t new_prot);

#endif
