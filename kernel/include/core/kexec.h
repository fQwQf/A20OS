#ifndef _CORE_KEXEC_H
#define _CORE_KEXEC_H

#include "core/types.h"
#include "mm/elf_rust.h"

#define KEXEC_MAX_SEGMENTS 16
#define KEXEC_MAX_IMAGE_SIZE (64UL << 20)

#define KEXEC_STATUS_EMPTY     0
#define KEXEC_STATUS_LOADED    1
#define KEXEC_STATUS_EXECUTING 2

typedef struct {
    const void *buf;
    size_t      bufsz;
    const void *mem;
    size_t      memsz;
} kexec_segment_t;

typedef struct {
    uint32_t state;
    uint32_t backend;
    uint64_t entry;
    uint32_t nr_segments;
    uint32_t reserved;
} kexec_status_t;

int kexec_load_segments(uint64_t entry, uint64_t nr_segments,
                        const void *usegments, unsigned long flags);
int kexec_load_segment_array(uint64_t entry, uint64_t nr_segments,
                             const kexec_segment_t *segments);
int kexec_load_image(const void *image, uint64_t size);
int kexec_load_file(int kernel_fd, int initrd_fd,
                    uint64_t cmdline_len, const void *ucmdline,
                    unsigned long flags);
int kexec_execute(void);
int kexec_discard(void);
int kexec_is_loaded(void);
void kexec_get_status(kexec_status_t *out);
const a20_elf_kexec_ledger_t *kexec_image_for_machine(void);

uint32_t machine_kexec_backend_id(void);
int machine_kexec_prepare(const a20_elf_kexec_ledger_t *image);
void machine_kexec_cancel(void);
void machine_kexec(const a20_elf_kexec_ledger_t *image) __attribute__((noreturn));

#endif
