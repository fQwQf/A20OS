#ifndef _HYP_GUEST_H
#define _HYP_GUEST_H

/*
 * The user-space guest loader behind both /hyp_boot and /hypvm.
 *
 * Two programs need the same four things and neither of them is interesting:
 * reading a file whole, walking an ELF64 into guest RAM at its link-time
 * physical addresses, building the minimal FDT an A20OS guest needs, and
 * making the eight hyp_* syscalls in the right order.  /hyp_boot (the gate's
 * fixed-parameter program) and /hypvm (the user tool that takes the same
 * knobs from argv) differ only in where their parameters come from and in the
 * wording of their own output, so the mechanics live here once.
 *
 * This module prints nothing: every line either program shows the user is
 * theirs to word, and a shared prefix would be a shared prefix to keep in sync
 * for no reason.  What it does return is everything a caller needs to explain
 * a failure -- see hyp_guest_call_error() for the syscall side and the out
 * parameters of hyp_guest_load_elf()/hyp_guest_fdt_build() for the rest.
 *
 * Everything here is riscv64-guest specific but arch-neutral to build: the ELF
 * walk insists on EM_RISCV because that is the only guest this slice boots.
 */

#include <stddef.h>
#include <stdint.h>

#define HYP_GUEST_PAGE_SIZE 4096UL

/* hyp_exit_reason_t, kernel/include/hyp/hyp_vcpu.h.  Copied rather than
 * included: that header is kernel-side and pulls in kernel/hyp/hyp.h. */
#define HYP_GUEST_EXIT_NONE     0
#define HYP_GUEST_EXIT_SHUTDOWN 1
#define HYP_GUEST_EXIT_FAULT    2
#define HYP_GUEST_EXIT_ERROR    3

/* "shutdown" / "fault" / "error" / "none", for the exit=N(name) reports. */
const char *hyp_guest_exit_name(long exit_reason);

/* Mirror of struct hyp_vm_status in kernel/abi/linux/sys_a20_bridge.c. */
struct hyp_guest_status {
    uint64_t exit;
    uint64_t scause;
    uint64_t stval;
    uint64_t htval;
    uint64_t marker_seen;
    uint64_t console_bytes;
};

/* ---- files ---- */

/* Read a whole file into a malloc'd buffer (NUL-terminated one byte past the
 * end, so it can be handed to strcmp-style code by callers that want to).
 * NULL on any error with errno left alone for the caller to report. */
unsigned char *hyp_guest_read_file(const char *path, size_t *out_len);

/* ---- ELF ---- */

/* Lay every PT_LOAD segment of a riscv64 ET_EXEC image into vm at its LMA.
 * Returns the number of segments loaded, or -1 if the image is not one this
 * loader can lay down (not ELF64/LSB, not ET_EXEC, not EM_RISCV, a program
 * header table that runs off the end, a PT_LOAD whose file bytes run off the
 * end, or a segment whose unaligned LMA would share its first page with
 * whatever precedes it).
 *
 * *entry_gpa is e_entry translated through the program headers -- the guest is
 * entered at the LMA, not at the virtual address the header carries, because the
 * kernel is linked higher-half.  *image_end is the highest p_paddr + p_memsz,
 * which is what the caller compares the DTB address against. */
int hyp_guest_load_elf(long vm, const unsigned char *img, size_t size,
                       uint64_t *entry_gpa, uint64_t *image_end);

/* ---- FDT ---- */

/* Upper bound on the blob hyp_guest_fdt_build() can produce.  Callers size
 * their allocation with this and check the build's return against it. */
size_t hyp_guest_fdt_capacity(void);

/* Build the minimum device tree an A20OS guest needs into out (which must hold
 * hyp_guest_fdt_capacity() bytes) and return its length, or 0 on failure.  The
 * tree describes RAM at [mem_base, mem_base + mem_size) and puts bootargs in
 * /chosen, which is where fdt_extract_bootargs() and the board's memory init
 * look; see the long comment above the builder for why those two and the
 * riscv,isa string are the whole list. */
size_t hyp_guest_fdt_build(unsigned char *out, size_t cap,
                           uint64_t mem_base, uint64_t mem_size,
                           const char *bootargs);

/* ---- syscalls ---- */

/* Thin wrappers over the Linux ABI bridge.  All of them return what syscall()
 * returns -- a handle >= 0, or -1 with errno set -- so a caller can report the
 * failure the way any other syscall failure is reported.  The kernel side is
 * kernel/abi/linux/sys_a20_bridge.c; the numbering and the reasons a Linux-ABI
 * task is the one that calls them are argued in hyp_boot.c's header comment. */
long hyp_guest_vm_create(uint64_t mem_size);
int  hyp_guest_vm_load(long vm, uint64_t gpa, const void *src, uint64_t len);
long hyp_guest_vcpu_create(long vm, uint64_t entry_gpa);
int  hyp_guest_vcpu_set_boot(long vcpu, uint64_t hartid, uint64_t dtb_gpa);
int  hyp_guest_vm_set_marker(long vm, const char *marker);
long hyp_guest_vcpu_run(long vcpu);
long hyp_guest_vm_status(long vm, struct hyp_guest_status *st);
int  hyp_guest_vm_destroy(long vm);

/* What to say about a syscall that came back -1.  Returns NULL when the kernel
 * named a plain operational failure (the caller reports rc and errno itself),
 * and a fixed sentence when the refusal means "this kernel cannot do that at
 * all": ENOTSUP when the CPU has no H extension -- the case that turns a boot
 * gate into a green light over nothing, so it gets said out loud -- and ENOSYS
 * when the running kernel predates the hyp calls. */
const char *hyp_guest_call_error(long rc);

#endif /* _HYP_GUEST_H */