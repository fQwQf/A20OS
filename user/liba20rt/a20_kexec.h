#ifndef _A20_KEXEC_H
#define _A20_KEXEC_H

#include "a20_types.h"
#include "a20_syscall.h"

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

static inline a20_status_t a20_kexec_load_image(const void *image, uint64_t size)
{
    return a20_syscall6(A20_SYS_system_kexec, A20_KEXEC_LOAD_IMAGE,
                        (uint64_t)(uintptr_t)image, size, 0, 0, 0);
}

static inline a20_status_t a20_kexec_load_segments(uint64_t entry,
        const a20_kexec_segment_t *segments, uint64_t count)
{
    return a20_syscall6(A20_SYS_system_kexec, A20_KEXEC_LOAD_SEGMENTS, entry,
                        (uint64_t)(uintptr_t)segments, count, 0, 0);
}

static inline a20_status_t a20_kexec_execute(void)
{
    return a20_syscall6(A20_SYS_system_kexec, A20_KEXEC_EXECUTE, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_kexec_discard(void)
{
    return a20_syscall6(A20_SYS_system_kexec, A20_KEXEC_DISCARD, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_kexec_status(a20_kexec_status_t *status)
{
    status->size = sizeof(*status);
    status->struct_version = 1;
    return a20_syscall6(A20_SYS_system_kexec, A20_KEXEC_STATUS,
                        (uint64_t)(uintptr_t)status, 0, 0, 0, 0);
}

#endif
