#ifndef _CORE_KEXEC_BACKEND_H
#define _CORE_KEXEC_BACKEND_H

#include "core/kexec.h"

typedef struct {
    uint64_t pages;
    uint64_t mem;
    uint64_t bufsz;
    uint64_t memsz;
} kexec_handoff_segment_t;

typedef struct {
    uint64_t entry;
    uint64_t boot_arg0;
    uint64_t boot_arg1;
    uint32_t nr;
    uint32_t stop_ack;
    uint64_t boot_arg2;
    /* Offsets are hardcoded in every arch trampoline (arch/<arch>/boot/
     * kexec.S): entry@0, boot_arg0@8, boot_arg1@16, nr@24, stop_ack@28,
     * boot_arg2@32, seg[]@40 with a 32-byte stride.  Any layout change must
     * update all of them. */
    kexec_handoff_segment_t seg[KEXEC_MAX_SEGMENTS];
} kexec_handoff_t;

int kexec_handoff_build(const a20_elf_kexec_ledger_t *image,
                        kexec_handoff_t *control, uint64_t control_pa,
                        uint64_t boot_arg0, uint64_t boot_arg1);
int kexec_bind_boot_cpu(uint32_t *saved_affinity, int *changed);
void kexec_restore_affinity(uint32_t saved_affinity, int changed);

#endif
