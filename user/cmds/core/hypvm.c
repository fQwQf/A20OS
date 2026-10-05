/*
 * hypvm -- run an A20OS virtual machine on the hypervisor, from the shell.
 *
 * /hyp_boot (hyp_boot.c) is the same boot with every parameter compiled in and
 * is the gate smoke-hyp-a20os names.  This is the user-facing one: the guest
 * ELF, the RAM window, the guest's /chosen/bootargs, the console marker and the
 * entry GPA are arguments, and the defaults are exactly the values hyp_boot
 * carries, so a bare `hypvm` boots what `hyp_boot` boots.
 *
 * The mechanics -- ELF walk, minimal FDT, the eight hyp_* syscalls -- are
 * shared with hyp_boot in hyp/hyp_guest.c.  What differs is what this program
 * has to decide for itself, and each decision below exists because the
 * parameter made it possible to get wrong:
 *
 *   - the FDT goes relative to the top of the RAM window (DTB_RESERVE bytes
 *     below base+mem) rather than at a fixed address.  hyp_boot's 0x87f00000
 *     is correct for exactly one window size: shrink the window with -m and
 *     that address lands inside the kernel image, which entry.S wipes with its
 *     own `la` stores before it ever reads a1.
 *   - the window base is an argument, so it is checked for the two things that
 *     make a guest address usable at all: page alignment (hyp_vm_load takes
 *     whole stage-2 pages) and the identity map entry.S lays down over
 *     BOOT_MAP_PHYS .. +16 GiB, without which __boot_dtb_ptr is not
 *     dereferenceable once the guest has installed its own satp
 *     (hyp_vcpu.h §"Guest boot protocol", docs/hypervisor/01-a20os-guest.md
 *     §7.4).
 *
 * PASS is the same judgement hyp_boot makes and for the same reason: the guest
 * console marker was seen, counted by the device model over guest UART bytes
 * only, because the host and the guest are the same kernel and print the same
 * banner on one shared console.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hyp/hyp_guest.h"

/* Defaults: hyp_boot's compile-time constants, one for one. */
#define HYPVM_ELF_PATH   "/bin/boot/guest-kernel.elf"
#define HYPVM_BASE       0x80000000ULL
#define HYPVM_MEM_MIB    128ULL
#define HYPVM_BOOTARGS   "a20.hypguest=1"
#define HYPVM_MARKER     "A20OS Kernel"

/* RAM kept above the image for the synthesized FDT, measured down from the top
 * of the window.  The blob itself is < 2 KiB (hyp_guest_fdt_capacity()), so a
 * mebibyte is slack rather than a requirement -- what it buys is that the
 * address does not move when bootargs grows. */
#define HYPVM_DTB_RESERVE (1ULL << 20)

/* entry.S identity-maps sixteen 1 GiB slots from BOOT_MAP_PHYS
 * (entry.S:132); a DTB outside that window faults the guest before it prints. */
#define HYPVM_BOOT_MAP_BASE 0x80000000ULL
#define HYPVM_BOOT_MAP_SIZE (16ULL << 30)

/* hyp_vm_set_marker() takes <= 32 bytes (hyp_vcpu.h).  A longer marker is
 * silently truncated there, which would make the marker the report claims and
 * the marker actually matched two different strings. */
#define HYPVM_MARKER_MAX 32

/* Exit codes, also in the usage text.  They are part of the tool's interface:
 * a script wants to tell "you typed it wrong" from "this kernel cannot do it"
 * from "the guest ran and did not arrive". */
#define HYPVM_EXIT_OK       0
#define HYPVM_EXIT_VERDICT  1  /* the guest ran; the marker verdict failed */
#define HYPVM_EXIT_USAGE    2  /* bad arguments */
#define HYPVM_EXIT_NOELF    3  /* the guest ELF is missing or unreadable */
#define HYPVM_EXIT_UNSUP    4  /* the kernel refused the hyp syscalls */
#define HYPVM_EXIT_LAYOUT   5  /* image/entry/DTB do not fit the window */

static void usage(const char *prog)
{
    printf("Usage: %s [-k <guest-kernel.elf>] [-m MiB] [-g gpa_base] "
           "[-b bootargs]\n"
           "       [--marker <str>] [-e guest_entry_gpa] [-h]\n\n",
           prog);
    printf("Boot an A20OS kernel as a hypervisor guest and report whether it\n"
           "reached its own console banner.\n\n");
    printf("  -k <path>   guest kernel ELF (default %s)\n", HYPVM_ELF_PATH);
    printf("  -m <MiB>    guest RAM window size (default %llu)\n",
           (unsigned long long)HYPVM_MEM_MIB);
    printf("  -g <gpa>    window base, page aligned (default 0x%llx)\n",
           (unsigned long long)HYPVM_BASE);
    printf("  -b <str>    /chosen/bootargs of the synthesized FDT "
           "(default %s)\n", HYPVM_BOOTARGS);
    printf("  --marker <s>  console string that counts as arrival, 1..%d bytes\n"
           "                (default %s)\n", HYPVM_MARKER_MAX, HYPVM_MARKER);
    printf("  -e <gpa>    guest entry GPA (default: e_entry translated "
           "through the\n"
           "                program headers, i.e. the image's LMA)\n");
    printf("  -h          this text\n\n");
    printf("Exit codes: %d pass, %d guest ran but the marker was not seen,\n"
           "            %d bad arguments, %d guest ELF unreadable,\n"
           "            %d kernel has no hyp syscalls (no H extension?),\n"
           "            %d image/entry/DTB do not fit the window.\n",
           HYPVM_EXIT_OK, HYPVM_EXIT_VERDICT, HYPVM_EXIT_USAGE,
           HYPVM_EXIT_NOELF, HYPVM_EXIT_UNSUP, HYPVM_EXIT_LAYOUT);
}

/* A syscall that came back -1 with errno==ENOTSUP/ENOSYS is a different kind
 * of failure from a VM that would not allocate, and it is the one that must
 * never be reported as a boot attempt: hypvm with no H extension measures
 * nothing at all. */
static int fail_call(const char *what, long rc)
{
    const char *why = hyp_guest_call_error(rc);
    printf("HYPVM: FAIL %s (rc=%ld errno=%d)%s%s\n", what, rc, errno,
           why ? ": " : "", why ? why : "");
    return HYPVM_EXIT_UNSUP;
}

static int failf(const char *fmt, ...)
{
    va_list ap;
    printf("HYPVM: FAIL ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    return HYPVM_EXIT_VERDICT;
}

/* strtoull with base 0 (so 0x... works) and no partial parses: a typo must be
 * a rejected argument, not a silently zero window. */
static int parse_u64(const char *s, uint64_t *out)
{
    if (!s || !*s || *s == '-' || *s == '+')
        return -1;
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno || !end || *end)
        return -1;
    *out = (uint64_t)v;
    return 0;
}

int main(int argc, char **argv)
{
    const char *elf_path = HYPVM_ELF_PATH;
    const char *bootargs = HYPVM_BOOTARGS;
    const char *marker   = HYPVM_MARKER;
    uint64_t mem_mib = HYPVM_MEM_MIB;
    uint64_t base    = HYPVM_BASE;
    uint64_t entry_arg = 0;
    int have_entry = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            return HYPVM_EXIT_OK;
        } else if (!strcmp(a, "-k")) {
            if (++i >= argc) { printf("HYPVM: FAIL -k needs a path\n");
                               return HYPVM_EXIT_USAGE; }
            elf_path = argv[i];
        } else if (!strcmp(a, "-b")) {
            if (++i >= argc) { printf("HYPVM: FAIL -b needs a string\n");
                               return HYPVM_EXIT_USAGE; }
            bootargs = argv[i];
        } else if (!strcmp(a, "--marker")) {
            if (++i >= argc) { printf("HYPVM: FAIL --marker needs a string\n");
                               return HYPVM_EXIT_USAGE; }
            marker = argv[i];
        } else if (!strcmp(a, "-m")) {
            if (++i >= argc || parse_u64(argv[i], &mem_mib)) {
                printf("HYPVM: FAIL -m needs a MiB count\n");
                return HYPVM_EXIT_USAGE;
            }
        } else if (!strcmp(a, "-g")) {
            if (++i >= argc || parse_u64(argv[i], &base)) {
                printf("HYPVM: FAIL -g needs a guest physical address\n");
                return HYPVM_EXIT_USAGE;
            }
        } else if (!strcmp(a, "-e")) {
            if (++i >= argc || parse_u64(argv[i], &entry_arg)) {
                printf("HYPVM: FAIL -e needs a guest physical address\n");
                return HYPVM_EXIT_USAGE;
            }
            have_entry = 1;
        } else {
            printf("HYPVM: FAIL unknown argument '%s'\n", a);
            usage(argv[0]);
            return HYPVM_EXIT_USAGE;
        }
    }

    /* The marker is matched by the device model over guest console bytes; it
     * has to be something, and it has to be short enough not to be truncated
     * into a different string than the one this program reports. */
    size_t marker_len = strlen(marker);
    if (marker_len == 0 || marker_len > HYPVM_MARKER_MAX) {
        printf("HYPVM: FAIL marker must be 1..%d bytes, got %lu\n",
               HYPVM_MARKER_MAX, (unsigned long)marker_len);
        return HYPVM_EXIT_USAGE;
    }

    /* Two MiB is the floor for a window this tool can lay a DTB into at all;
     * the real limit is the image, which is checked once it has been read. */
    if (mem_mib < 2) {
        printf("HYPVM: FAIL -m %llu is below the 2 MiB floor\n",
               (unsigned long long)mem_mib);
        return HYPVM_EXIT_USAGE;
    }
    uint64_t mem = mem_mib << 20;
    if (base & (HYP_GUEST_PAGE_SIZE - 1)) {
        printf("HYPVM: FAIL -g 0x%llx is not page aligned\n",
               (unsigned long long)base);
        return HYPVM_EXIT_USAGE;
    }
    if (base < HYPVM_BOOT_MAP_BASE ||
        base + mem > HYPVM_BOOT_MAP_BASE + HYPVM_BOOT_MAP_SIZE) {
        /* Not fatal in itself, but it is the difference between a guest that
         * prints and one that faults on the first DTB read, and it is invisible
         * from the guest side -- so it is said here, before the run. */
        printf("HYPVM: FAIL -g 0x%llx + %llu MiB is outside the identity map "
               "entry.S lays down (0x%llx .. +%llu GiB)\n",
               (unsigned long long)base, (unsigned long long)mem_mib,
               (unsigned long long)HYPVM_BOOT_MAP_BASE,
               (unsigned long long)(HYPVM_BOOT_MAP_SIZE >> 30));
        return HYPVM_EXIT_LAYOUT;
    }

    size_t img_len = 0;
    unsigned char *img = hyp_guest_read_file(elf_path, &img_len);
    if (!img) {
        printf("HYPVM: FAIL cannot read %s (errno=%d)\n", elf_path, errno);
        return HYPVM_EXIT_NOELF;
    }

    size_t dtb_cap = hyp_guest_fdt_capacity();
    unsigned char *dtb = malloc(dtb_cap);
    if (!dtb) {
        free(img);
        printf("HYPVM: FAIL dtb alloc\n");
        return HYPVM_EXIT_VERDICT;
    }
    size_t dtb_len = hyp_guest_fdt_build(dtb, dtb_cap, base, mem, bootargs);
    if (!dtb_len) {
        free(img);
        free(dtb);
        printf("HYPVM: FAIL dtb build\n");
        return HYPVM_EXIT_LAYOUT;
    }

    printf("HYPVM: guest=%s elf_bytes=%lu mem=%llu MiB base=0x%llx "
           "marker='%s' bootargs='%s'\n",
           elf_path, (unsigned long)img_len, (unsigned long long)mem_mib,
           (unsigned long long)base, marker, bootargs);

    long vm = hyp_guest_vm_create(mem);
    if (vm < 0) {
        free(img);
        free(dtb);
        return fail_call("vm_create", vm);
    }

    /* The ELF walk loads through the VM, so it happens after the VM exists; a
     * malformed image is reported and the VM dropped. */
    uint64_t entry_gpa = 0, image_end = 0;
    int nseg = hyp_guest_load_elf(vm, img, img_len, &entry_gpa, &image_end);
    if (nseg < 0) {
        hyp_guest_vm_destroy(vm);
        free(img);
        free(dtb);
        printf("HYPVM: FAIL guest ELF rejected (entry=0x%llx)\n",
               (unsigned long long)entry_gpa);
        return HYPVM_EXIT_LAYOUT;
    }
    if (have_entry) {
        if (entry_arg < base || entry_arg >= base + mem) {
            hyp_guest_vm_destroy(vm);
            free(img);
            free(dtb);
            printf("HYPVM: FAIL -e 0x%llx is outside the RAM window "
                   "[0x%llx, 0x%llx)\n", (unsigned long long)entry_arg,
                   (unsigned long long)base,
                   (unsigned long long)(base + mem));
            return HYPVM_EXIT_LAYOUT;
        }
        entry_gpa = entry_arg;
    }
    if (image_end > base + mem) {
        hyp_guest_vm_destroy(vm);
        free(img);
        free(dtb);
        printf("HYPVM: FAIL image end 0x%llx past RAM window end 0x%llx\n",
               (unsigned long long)image_end,
               (unsigned long long)(base + mem));
        return HYPVM_EXIT_LAYOUT;
    }

    /* The DTB sits at the top of the window, page aligned down so an odd -g
     * cannot make it straddle a page (hyp_vm_load takes whole pages). */
    uint64_t dtb_gpa = (base + mem - HYPVM_DTB_RESERVE) &
                       ~(uint64_t)(HYP_GUEST_PAGE_SIZE - 1);
    if (dtb_gpa + dtb_len > base + mem) {
        hyp_guest_vm_destroy(vm);
        free(img);
        free(dtb);
        printf("HYPVM: FAIL dtb does not fit below 0x%llx\n",
               (unsigned long long)(base + mem));
        return HYPVM_EXIT_LAYOUT;
    }
    if (dtb_gpa < image_end) {
        hyp_guest_vm_destroy(vm);
        free(img);
        free(dtb);
        printf("HYPVM: FAIL dtb gpa 0x%llx overlaps the image (ends 0x%llx); "
               "the window is too small for this guest\n",
               (unsigned long long)dtb_gpa, (unsigned long long)image_end);
        return HYPVM_EXIT_LAYOUT;
    }

    if (hyp_guest_vm_load(vm, dtb_gpa, dtb, dtb_len) < 0) {
        hyp_guest_vm_destroy(vm);
        free(img);
        free(dtb);
        return fail_call("dtb load", -1);
    }

    long vcpu = hyp_guest_vcpu_create(vm, entry_gpa);
    if (vcpu < 0) {
        hyp_guest_vm_destroy(vm);
        free(img);
        free(dtb);
        return fail_call("vcpu_create", vcpu);
    }

    /* hartid 0 is not a default: entry.S branches past its SBI HSM lottery
     * handoff only on a0 == 0 (entry.S:44), and the vcpu slice has one hart. */
    if (hyp_guest_vcpu_set_boot(vcpu, 0, dtb_gpa) < 0) {
        hyp_guest_vm_destroy(vm);
        free(img);
        free(dtb);
        return fail_call("vcpu_set_boot", -1);
    }

    if (hyp_guest_vm_set_marker(vm, marker) < 0) {
        hyp_guest_vm_destroy(vm);
        free(img);
        free(dtb);
        return fail_call("vm_set_marker", -1);
    }

    printf("HYPVM: segments=%d entry_gpa=0x%llx image_end=0x%llx dtb_gpa=0x%llx\n",
           nseg, (unsigned long long)entry_gpa, (unsigned long long)image_end,
           (unsigned long long)dtb_gpa);
    printf("HYPVM: running\n");
    fflush(stdout);

    long exit_reason = hyp_guest_vcpu_run(vcpu);

    struct hyp_guest_status st;
    memset(&st, 0, sizeof(st));
    long r = hyp_guest_vm_status(vm, &st);

    hyp_guest_vm_destroy(vm);
    free(img);
    free(dtb);

    if (exit_reason < 0)
        return fail_call("vcpu_run", exit_reason);
    if (r < 0)
        return fail_call("vm_status", r);

    printf("HYPVM: exit=%ld(%s) scause=0x%llx stval=0x%llx htval=0x%llx "
           "marker_seen=%llu console_bytes=%llu\n",
           exit_reason, hyp_guest_exit_name(exit_reason),
           (unsigned long long)st.scause, (unsigned long long)st.stval,
           (unsigned long long)st.htval,
           (unsigned long long)st.marker_seen,
           (unsigned long long)st.console_bytes);

    /* Same two judgements hyp_boot makes: the marker was matched over guest
     * console bytes (so the host's own identical banner cannot satisfy it),
     * and it was not a degenerate hit against a guest that printed nothing
     * else. */
    if (!st.marker_seen)
        return failf("guest never printed '%s' (exit=%ld)", marker, exit_reason);
    if (st.console_bytes <= (uint64_t)marker_len)
        return failf("guest wrote %llu bytes, marker is %lu: hit is degenerate",
                     (unsigned long long)st.console_bytes,
                     (unsigned long)marker_len);

    printf("HYPVM: PASS marker_seen=%llu console_bytes=%llu exit=%ld(%s) "
           "mem=%llu MiB\n",
           (unsigned long long)st.marker_seen,
           (unsigned long long)st.console_bytes, exit_reason,
           hyp_guest_exit_name(exit_reason), (unsigned long long)mem_mib);
    return HYPVM_EXIT_OK;
}