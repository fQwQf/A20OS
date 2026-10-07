/*
 * hyp_boot -- boot a real A20OS kernel as a hypervisor guest.
 *
 * The vcpu slice's own gate (hyp_test.c) runs 56 bytes of hand-assembled SBI
 * calls.  This one boots the same kernel the host is running: it reads the
 * guest ELF out of the host filesystem, lays its PT_LOAD segments into guest
 * RAM at their link-time physical addresses, hands the vcpu a minimal device
 * tree, and lets it run until it leaves.
 *
 * PASS is "the guest reached its own banner", not "the guest booted": a guest
 * with no disk cannot mount a rootfs and dies in init_kthread, which is a real
 * arrival, not a failure.  What would be a failure is the guest never printing
 * -- indistinguishable, on one shared console, from a host that has quietly
 * stopped.  So the verdict is taken from marker_seen, which the device model
 * counts over guest console bytes only (hyp_vcpu.h), and never from parsing
 * interleaved log text.
 *
 * This program is the fixed-parameter boot gate and stays that way: the smoke
 * smoke-hyp-a20os names its output in tools/smoke_cases.py, so its defaults are
 * part of the contract and its wording is not to be reworded.  The same loader
 * with the parameters taken from argv is /hypvm (hypvm.c); the mechanics both
 * of them share -- the ELF walk, the minimal FDT, the syscall wrappers -- live
 * in hyp/hyp_guest.c, and the reasoning that lives with that code (LMA vs
 * e_entry, the .bss tail, why the DTB has to be identity-reachable) moved with
 * it.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hyp/hyp_guest.h"

/* The FAT32 utilities image is mounted at /bin in the default (non
 * EXTERNAL_ROOT) boot -- kernel/fs/mount_setup.c mount_block_devices() picks
 * utilities_path = "/bin".  The guest ELF is written at the image root
 * (::/boot/guest-kernel.elf, tools/img.py), which in the running system is
 * /bin/boot/guest-kernel.elf, the same convention the other user-space tests
 * use for image-root paths. */
#define GUEST_ELF_PATH "/bin/boot/guest-kernel.elf"

/* Stage-2 RAM window default, hyp_vcpu.h: [0x80000000, base + mem_size).  The
 * kernel image (text+rodata+data+bss) ends around 0x810A0914, so 128 MiB holds
 * the image, the boot page tables and the stacks without pretending the guest
 * owns host memory it will never touch. */
#define GUEST_BASE 0x80000000ULL
#define GUEST_MEM  (128ULL << 20)

/* Where the loader's own FDT goes.  Two constraints, neither visible in the
 * code below:
 *
 *  - clear of the image (which reaches ~0x810D08EC), because entry.S clears
 *    BSS itself with `la` stores before it reads a1, and that would wipe a DTB
 *    parked inside the image;
 *  - inside the window entry.S identity-maps (BOOT_MAP_PHYS = 0x80000000,
 *    sixteen 1 GiB slots, entry.S:132).  a1 is a physical address by the SBI
 *    convention, and __boot_dtb_ptr is dereferenced as a plain pointer by
 *    riscv64_memory_init() and fdt_extract_bootargs() -- after the guest has
 *    installed its own satp.  A GPA is only still dereferenceable after that
 *    if the guest's identity map covers it, so a DTB outside the boot identity
 *    window faults the guest before it prints anything.
 */
#define GUEST_DTB_GPA 0x87f00000ULL

/* /chosen/bootargs of the synthesized FDT.  a20.hypguest=1 is what lets a
 * reader tell guest output from host output in a shared console; see the
 * comment above hyp_guest_fdt_build(). */
#define GUEST_BOOTARGS "a20.hypguest=1"

/* kernel/main.c prints "    A20OS Kernel \n" as the first thing kernel_main
 * does, before uart_init and long before any SBI call, and the guest writes it
 * to the 16550 at 0x10000000 directly (arch_uart_putc), so it is counted by the
 * device model's console byte counter.  A prefix of the host's own identical
 * banner cannot be confused with it: the marker is scanned over guest bytes
 * only.  Log evidence: .kernel-build/smoke/mm-stress-riscv64.log line 66. */
#define GUEST_MARKER "A20OS Kernel"

static int fail(const char *what, long rc)
{
    printf("HYP_A20OS: FAIL %s (rc=%ld errno=%d)\n", what, rc, errno);
    return 1;
}

static int failf(const char *fmt, ...)
{
    va_list ap;
    printf("HYP_A20OS: FAIL ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    return 1;
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    unsigned char *dtb = malloc(hyp_guest_fdt_capacity());
    if (!dtb)
        return fail("dtb alloc", -1);

    size_t dtb_len = hyp_guest_fdt_build(dtb, hyp_guest_fdt_capacity(),
                                         GUEST_BASE, GUEST_MEM, GUEST_BOOTARGS);
    if (!dtb_len)
        return fail("dtb build", -1);

    size_t img_len = 0;
    unsigned char *img = hyp_guest_read_file(GUEST_ELF_PATH, &img_len);
    if (!img)
        return failf("cannot read %s (errno=%d)", GUEST_ELF_PATH, errno);
    printf("HYP_A20OS: guest=%s size=%lu dtb=%lu\n",
           GUEST_ELF_PATH, (unsigned long)img_len, (unsigned long)dtb_len);

    long vm = hyp_guest_vm_create(GUEST_MEM);
    if (vm < 0) {
        free(img);
        free(dtb);
        return fail("vm_create", vm);
    }

    /* The ELF walk loads through the VM, so it happens after the VM exists; a
     * malformed image is reported and the VM dropped. */
    long vcpu;
    uint64_t entry_gpa = 0, image_end = 0;
    int nseg = hyp_guest_load_elf(vm, img, img_len, &entry_gpa, &image_end);
    if (nseg < 0) {
        hyp_guest_vm_destroy(vm);
        free(img);
        free(dtb);
        return failf("guest ELF rejected (entry=0x%llx)",
                     (unsigned long long)entry_gpa);
    }
    if (image_end > GUEST_BASE + GUEST_MEM) {
        hyp_guest_vm_destroy(vm);
        free(img);
        free(dtb);
        return failf("image end 0x%llx past RAM window end 0x%llx",
                     (unsigned long long)image_end,
                     (unsigned long long)(GUEST_BASE + GUEST_MEM));
    }
    if (GUEST_DTB_GPA < image_end) {
        hyp_guest_vm_destroy(vm);
        free(img);
        free(dtb);
        return failf("dtb gpa 0x%llx overlaps the image",
                     (unsigned long long)GUEST_DTB_GPA);
    }

    if (hyp_guest_vm_load(vm, GUEST_DTB_GPA, dtb, dtb_len) < 0) {
        hyp_guest_vm_destroy(vm);
        free(img);
        free(dtb);
        return fail("dtb load", -1);
    }

    vcpu = hyp_guest_vcpu_create(vm, entry_gpa);
    if (vcpu < 0) {
        hyp_guest_vm_destroy(vm);
        free(img);
        free(dtb);
        return fail("vcpu_create", vcpu);
    }

    /* hartid 0 is not a default, it is the value the guest entry code needs:
     * entry.S branches straight past its SBI HSM lottery handoff on a0 == 0
     * (entry.S:44), and the vcpu slice has exactly one hart to be. */
    if (hyp_guest_vcpu_set_boot(vcpu, 0, GUEST_DTB_GPA) < 0) {
        hyp_guest_vm_destroy(vm);
        free(img);
        free(dtb);
        return fail("vcpu_set_boot", -1);
    }

    if (hyp_guest_vm_set_marker(vm, GUEST_MARKER) < 0) {
        hyp_guest_vm_destroy(vm);
        free(img);
        free(dtb);
        return fail("vm_set_marker", -1);
    }

    printf("HYP_A20OS: segments=%d entry_gpa=0x%llx image_end=0x%llx "
           "ram=%lu MiB marker='%s'\n",
           nseg, (unsigned long long)entry_gpa, (unsigned long long)image_end,
           (unsigned long)(GUEST_MEM >> 20), GUEST_MARKER);
    printf("HYP_A20OS: running\n");
    fflush(stdout);

    long exit_reason = hyp_guest_vcpu_run(vcpu);

    struct hyp_guest_status st;
    memset(&st, 0, sizeof(st));
    long r = hyp_guest_vm_status(vm, &st);

    hyp_guest_vm_destroy(vm);
    free(img);
    free(dtb);

    if (exit_reason < 0)
        return fail("vcpu_run", exit_reason);
    if (r < 0)
        return fail("vm_status", r);

    printf("HYP_A20OS: exit=%ld(%s) scause=0x%llx stval=0x%llx htval=0x%llx "
           "marker_seen=%llu console_bytes=%llu\n",
           exit_reason, hyp_guest_exit_name(exit_reason),
           (unsigned long long)st.scause, (unsigned long long)st.stval,
           (unsigned long long)st.htval,
           (unsigned long long)st.marker_seen,
           (unsigned long long)st.console_bytes);

    /* The marker is matched as a sliding window over guest console bytes, so a
     * hit already means the whole string arrived.  The byte count is checked
     * as well so a report can never read "12 bytes seen, 12-byte marker
     * matched" -- the guest wrote more than the banner and kept going. */
    if (!st.marker_seen)
        return failf("guest never printed '%s' (exit=%ld)", GUEST_MARKER,
                     exit_reason);
    if (st.console_bytes <= (uint64_t)strlen(GUEST_MARKER))
        return failf("guest wrote %llu bytes, marker is %u: hit is degenerate",
                     (unsigned long long)st.console_bytes,
                     (unsigned)strlen(GUEST_MARKER));

    printf("HYP_A20OS: PASS\n");
    return 0;
}