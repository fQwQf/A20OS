#ifndef _UAPI_A20_KEXEC_H
#define _UAPI_A20_KEXEC_H

#include "core/types.h"

#define A20_KEXEC_LOAD_IMAGE    1U
#define A20_KEXEC_LOAD_SEGMENTS 2U
#define A20_KEXEC_EXECUTE       3U
#define A20_KEXEC_DISCARD       4U
#define A20_KEXEC_STATUS        5U

#define A20_KEXEC_IMAGE_EMPTY     0U
#define A20_KEXEC_IMAGE_LOADED    1U
#define A20_KEXEC_IMAGE_EXECUTING 2U
#define A20_KEXEC_BACKEND_RISCV64_QEMU (1U << 0)

typedef struct {
    uint64_t buf;
    uint64_t bufsz;
    uint64_t mem;
    uint64_t memsz;
} a20_kexec_segment_t;

typedef struct {
    uint32_t size;
    uint32_t struct_version;
    uint32_t state;
    uint32_t backend;
    uint64_t entry;
    uint32_t nr_segments;
    uint32_t reserved;
} a20_kexec_status_t;

#endif
