#ifndef _IPC_KEXEC_H
#define _IPC_KEXEC_H

#include "core/types.h"

/*
 * kexec_load/kexec_file_load image staging (kernel/ipc/kexec.c).
 *
 * The image and its staging transaction are Rust-owned
 * (kernel/mm/elf_core.rs; the ledger type is a20_elf_kexec_ledger_t in
 * mm/elf_rust.h). This header keeps only the syscall-facing entry points and
 * the wire layout of the user-supplied struct kexec_segment.
 */

/* Wire layout of struct kexec_segment. Field widths are the target's own, so
 * an ILP32 guest sends 4-byte pointers and sizes; a fixed 8-byte stride would
 * misread every field there. */
typedef struct {
    const void *buf;
    size_t      bufsz;
    const void *mem;
    size_t      memsz;
} kexec_segment_t;

#define KEXEC_MAX_SEGMENTS 16

int  kexec_load_segments(uint64_t entry, uint64_t nr_segments,
                         const void *usegments, unsigned long flags);
int  kexec_load_file(int kernel_fd, int initrd_fd,
                     uint64_t cmdline_len, const void *ucmdline,
                     unsigned long flags);
int  kexec_is_loaded(void);
void kexec_discard(void);

#endif /* _IPC_KEXEC_H */
