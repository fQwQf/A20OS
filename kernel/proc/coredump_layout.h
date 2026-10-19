#ifndef A20_PROC_COREDUMP_LAYOUT_H
#define A20_PROC_COREDUMP_LAYOUT_H

#include "core/types.h"

/*
 * C/Rust ABI for the ELF core-dump layout in kernel/proc/coredump_layout.rs.
 *
 * Rust owns the file layout: the core_pattern expansion, the VM-flag to
 * p_flags mapping, the note blob (NT_PRSTATUS/NT_FPREGSET/NT_PRPSINFO with
 * their four-byte framing), the program-header table and the RLIMIT_CORE
 * clamping.  The adapter reads the task, snapshots the VMA list under
 * mm->lock, and performs the VFS writes.
 *
 * Every wire structure is fixed-width, so these layouts are identical on
 * every port that builds this file; the Rust mirror asserts the same sizes.
 */

#define COREDUMP_EHDR_SIZE 64
#define COREDUMP_PHDR_SIZE 56
#define COREDUMP_NAME_MAX  64

typedef struct coredump_note_input {
    int32_t  sig;
    int32_t  pid;
    int32_t  ppid;
    int32_t  pgid;
    int32_t  sid;
    uint32_t uid;
    uint32_t gid;
    uint64_t regs[32];
    uint64_t fp[32];
    uint64_t fcsr;
    char     name[COREDUMP_NAME_MAX];
} coredump_note_input_t;

typedef struct coredump_vma_input {
    uint64_t start;
    uint64_t end;
    uint64_t flags;
} coredump_vma_input_t;

typedef struct coredump_layout_result {
    uint64_t notes_len;
    uint64_t notes_off;
    uint64_t data_off;
    uint16_t phnum;
} coredump_layout_result_t;

typedef struct coredump_segment {
    uint64_t offset;
    uint64_t filesz;
} coredump_segment_t;

/* The u64 members are 8-byte aligned on every port that builds this file, so
 * the offsets below are architecture-independent; a port whose ABI disagrees
 * fails here instead of silently disagreeing with Rust. */
_Static_assert(__builtin_offsetof(coredump_note_input_t, regs) == 32,
               "coredump_note_input_t.regs offset");
_Static_assert(__builtin_offsetof(coredump_note_input_t, fp) == 288,
               "coredump_note_input_t.fp offset");
_Static_assert(__builtin_offsetof(coredump_note_input_t, fcsr) == 544,
               "coredump_note_input_t.fcsr offset");
_Static_assert(__builtin_offsetof(coredump_note_input_t, name) == 552,
               "coredump_note_input_t.name offset");

int a20_coredump_expand_pattern(const char *pattern, const char *name, int32_t pid,
                                int32_t tgid, int32_t uid, int32_t gid, int32_t sig,
                                uint64_t seconds, char *out, size_t outsz);
int a20_coredump_notes(const coredump_note_input_t *input, char *buf, size_t cap,
                       uint64_t *out_len);
int a20_coredump_layout(const coredump_vma_input_t *vmas, uint32_t nvma,
                        uint64_t notes_len, uint64_t rlim, uint64_t page_size,
                        uint16_t e_machine, char *ehdr_out, char *phdrs_out,
                        size_t phdrs_cap, coredump_segment_t *segs_out,
                        size_t segs_cap, coredump_layout_result_t *result);

#endif /* A20_PROC_COREDUMP_LAYOUT_H */
