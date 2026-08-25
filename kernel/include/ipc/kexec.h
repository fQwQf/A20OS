#ifndef _IPC_KEXEC_H
#define _IPC_KEXEC_H

#include "core/types.h"

/* kexec_load/kexec_file_load image staging (kernel/ipc/kexec.c). */

typedef struct {
    uint64_t bufsz;
    uint64_t memsz;
    uint64_t mem;      /* destination physical address */
    void   *pages;     /* chained allocated frames holding the copy */
    uint64_t pages_bytes;
} kexec_segment_staged_t;

#define KEXEC_MAX_SEGMENTS 16

typedef struct {
    int valid;
    int file_backed;
    uint64_t entry;
    int nr_segments;
    kexec_segment_staged_t seg[KEXEC_MAX_SEGMENTS];
} kexec_image_t;

int  kexec_load_segments(uint64_t entry, uint64_t nr_segments,
                         const void *usegments, unsigned long flags);
int  kexec_load_file(int kernel_fd, int initrd_fd,
                     uint64_t cmdline_len, const void *ucmdline,
                     unsigned long flags);
int  kexec_is_loaded(void);
void kexec_discard(void);

#endif /* _IPC_KEXEC_H */
