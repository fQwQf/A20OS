#ifndef _HYP_H
#define _HYP_H

#include "core/types.h"
#include "mm/pt.h"
#include "hyp/hyp_arch.h"

/*
 * Hypervisor foundation: guest physical address spaces (stage-2) and the VM
 * container around them.  Design record: docs/hypervisor/00-design.md.
 *
 * The load-bearing decision is that a stage-2 tree is a page table of the
 * SAME shape as a host one and reuses the single-level model's machinery
 * wholesale:
 *
 *   - every stage-2 page-table page carries a pt_meta_t (same node MCS lock,
 *     same per-entry status byte), so the concurrency and audit story of
 *     docs/roadmap/single-level-mm-model.md applies unchanged;
 *   - a mapped stage-2 leaf carries class MM_ST_GUEST_MEM -- never a host
 *     anon/file class, which would describe the wrong plane -- and the host
 *     auditor counts GUEST_MEM in a host table as a mismatch;
 *   - the frame a leaf names carries FRAME_F_GUEST for as long as the
 *     mapping exists (mm_pt_frame_lend/return), and mm_s2_audit()
 *     cross-checks flag against mapping so a frame whose VM died without
 *     returning it cannot hide;
 *   - hgatp is Sv39x4, but this file only ever programs root entries
 *     [0,512): guest physical memory must live below 512 GiB (HYP_GPA_LIMIT).
 *     The demo guest lives at 0x80000000; lifting the limit means an x4 root
 *     (2048 entries, 16 KiB) and a wider root metadata block, which is a
 *     format change to pt_meta_t and is deliberately deferred.
 *
 * Everything here runs in host kernel context on behalf of a host process.
 * The vcpu run loop (register save/restore, guest trap entry) is arch code;
 * see kernel/arch/riscv64/hyp/.
 */

/* Guest console marker, hyp_vcpu.h's bound.  The device model keeps the bytes
 * in the VM so the match state travels with the object the host sets up. */
#define HYP_VM_MARKER_MAX 32

/* Guest console ingress ring, in bytes.  One guest UART page's worth of input
 * is 256 bytes, which is also the HOST rx ring's size (RX_BUF_SIZE in
 * kernel/drivers/char/uart.c) -- the two cannot grow independently anyway,
 * since this ring is filled by draining the host one. */
#define HYP_VM_RX_RING 256

typedef struct hyp_vm {
    uint32_t magic;
#define HYP_VM_MAGIC 0x48594d56 /* 'HYMV' */
    pte_t    *s2_root;      /* stage-2 root; pt_meta_t attached (level 2) */
    uint16_t  vmid;         /* hgatp.VMID; TLB tag for this address space */
    uint64_t  mem_size;     /* guest RAM the VM is provisioned for */
    int       refcount;

    /* ---- v2 (hyp_vcpu.h): on-demand RAM window and console marker ----
     * Appended, never in front of the fields above: hyp_vcpu_asm.S pins no
     * offset into hyp_vm_t, but a stage-2 entry address derived from any of
     * the first five must not move.  marker_len / marker_pos / marker_seen /
     * console_bytes are touched from the guest trap path (no lock context) and
     * are therefore only ever reached through __atomic ops; see hyp_dev.c. */
    uint64_t  ram_base;       /* first GPA hyp_ram_fill() will serve */
    uint64_t  ram_size;       /* window length in bytes */
    uint64_t  console_bytes;  /* guest UART bytes since VM creation */
    uint32_t  marker_len;     /* valid bytes in marker[]; release/acquire */
    uint32_t  marker_pos;     /* sliding-match cursor into marker[] */
    int       marker_seen;    /* 1 once the marker matched in full */
    char      marker[HYP_VM_MARKER_MAX];

    /* ---- v3: guest console INPUT (host keystrokes -> guest UART RBR) ----
     * Appended on the same terms as the block above: nothing here is pinned by
     * hyp_vcpu_asm.S, and a stage-2 entry address derived from any of the
     * first five fields must not move.
     *
     * This is a plain single-producer/single-consumer ring with a one-slot
     * slack convention: head == tail is empty, so HYP_VM_RX_RING bytes of
     * storage carry HYP_VM_RX_RING-1.  The producer is hyp_dev_pump_rx() and
     * the consumer is the guest's own LSR/RBR read, both running on the CPU
     * that is running the guest (see hyp_dev.c for why that is a fact rather
     * than a hope), so the two indices need no lock; they are still written
     * with __atomic ops so the code does not depend on that for its memory
     * ordering.  rx_bytes is the lifetime ingress count, the ingress
     * counterpart of console_bytes. */
    uint8_t   rx_buf[HYP_VM_RX_RING];
    uint32_t  rx_head;      /* producer: next slot to write */
    uint32_t  rx_tail;      /* consumer: next slot to read  */
    uint64_t  rx_bytes;     /* host bytes handed to this guest, lifetime */
} hyp_vm_t;

/* Feature probe: 1 when the CPU implements the virtualization extension and
 * the arch layer can drive it.  Cheap enough to call per syscall. */
int  hyp_supported(void);

/* Create a VM with an empty stage-2.  mem_size is bookkeeping for now (the
 * guest RAM is provisioned page by page through hyp_s2_map); it caps nothing
 * yet and that is documented, not hidden.  Returns NULL on failure. */
hyp_vm_t *hyp_vm_create(uint64_t mem_size);
void      hyp_vm_put(hyp_vm_t *vm);

/* Map one 4 KiB guest page: gpa -> host frame, lending the frame to the VM.
 * prot is a host PTE flag set; PTE_U is stripped (stage-2 has no U bit).
 * Returns 0, or a negative errno.  -EEXIST when the gpa is already mapped. */
int  hyp_s2_map(hyp_vm_t *vm, uint64_t gpa, pfn_t pfn, pte_t prot);
/* Remove one 4 KiB guest page mapping, returning the frame. */
int  hyp_s2_unmap(hyp_vm_t *vm, uint64_t gpa);
/* Translate gpa -> host physical address, or 0 when unmapped.  Audit/test
 * helper; the guest hardware does its own walks. */
paddr_t hyp_s2_translate(hyp_vm_t *vm, uint64_t gpa);

/* Audit the stage-2 tree against its metadata (mm_s2_audit) plus the
 * frame-flag cross-check.  Returns 0 when clean. */
int  hyp_s2_audit(hyp_vm_t *vm, mm_pt_audit_report_t *out);

/* 1 when gpa is inside the VM's on-demand RAM window (hyp_vm_set_ram, set by
 * hyp_vm_create to [0x80000000, mem_size)).  The device model asks this so a
 * GPA the RAM path claims is never answered as an ignored MMIO access: a fill
 * that failed must stay a visible guest fault, not a silent drop. */
int  hyp_vm_ram_contains(hyp_vm_t *vm, uint64_t gpa);

/* Kernel-side selftest, run by reading /proc/a20/hyp_selftest.  Creates a
 * VM, maps and verifies pages, audits, unmaps, destroys, and writes one
 * HYP_SELFTEST line per step.  Returns 0 when every step passed. */
int  hyp_selftest(void);

#endif /* _HYP_H */
