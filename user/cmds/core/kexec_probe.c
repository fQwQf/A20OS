/*
 * Guest probe for the kexec_load() call path.
 *
 * The loader's kexec policy (segment count, wrap/overflow, kernel-image
 * overlap and pairwise overlap) is decided in Rust
 * (a20_elf_kexec_wire_ok / a20_elf_kexec_segment_ok); the kernel keeps the
 * usercopy, frame staging and rollback mechanics.  This probe drives the real
 * syscall end to end on the guest, not a host model:
 *
 *   - before anything is staged, reboot(2) with LINUX_REBOOT_CMD_KEXEC must
 *     answer -EINVAL; a staged image makes it answer -ENOSYS (no architecture
 *     kexec backend), which is how the probe proves the image really landed;
 *   - count, bufsz>memsz, out-of-count and pairwise-overlap segments must be
 *     refused with the documented errno;
 *   - a refused load must leave a previously staged image intact (rollback),
 *     and a later valid load must replace it (reset frees the old frames).
 *
 * Root (euid 0) satisfies the CAP_SYS_BOOT gate.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#define LINUX_REBOOT_MAGIC1       0xfee1deadUL
#define LINUX_REBOOT_MAGIC2       672274793UL
#define LINUX_REBOOT_CMD_KEXEC    0x45584543UL

struct kexec_segment_wire {
    const void *buf;
    size_t bufsz;
    const void *mem;
    size_t memsz;
};

static int failures;

static long reboot_kexec(void)
{
    return syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                   LINUX_REBOOT_CMD_KEXEC);
}

/* errno the probe expects from a refused reboot/kexec call. */
static void expect_reboot(const char *what, int want_errno)
{
    errno = 0;
    long rc = reboot_kexec();
    if (rc != -1 || errno != want_errno) {
        printf("KEXEC: FAIL %s rc=%ld errno=%d want=%d\n",
               what, rc, errno, want_errno);
        failures++;
    } else {
        printf("KEXEC: ok %s (errno=%d)\n", what, errno);
    }
}

static void expect_load(const char *what, unsigned long nr,
                        const struct kexec_segment_wire *segs, int want_errno)
{
    errno = 0;
    long rc = syscall(SYS_kexec_load, 0UL, nr, segs, 0UL);
    if (want_errno == 0) {
        if (rc != 0) {
            printf("KEXEC: FAIL %s rc=%ld errno=%d want=success\n",
                   what, rc, errno);
            failures++;
        } else {
            printf("KEXEC: ok %s (staged)\n", what);
        }
        return;
    }
    if (rc != -1 || errno != want_errno) {
        printf("KEXEC: FAIL %s rc=%ld errno=%d want=%d\n",
               what, rc, errno, want_errno);
        failures++;
    } else {
        printf("KEXEC: ok %s (errno=%d)\n", what, errno);
    }
}

int main(void)
{
    static unsigned char payload[4096];
    memset(payload, 0xa5, sizeof(payload));

    /* Physical placements well above the kernel image (0x8020_0000..). */
    const void *mem_a = (const void *)0x90000000UL;
    const void *mem_b = (const void *)0x90001000UL;
    const void *mem_overlap = (const void *)0x90000800UL;

    struct kexec_segment_wire one = {
        .buf = payload, .bufsz = sizeof(payload),
        .mem = mem_a, .memsz = sizeof(payload),
    };
    struct kexec_segment_wire bad_sizes = {
        .buf = payload, .bufsz = sizeof(payload),
        .mem = mem_a, .memsz = sizeof(payload) / 2,
    };
    struct kexec_segment_wire pair_ok[2] = {
        { .buf = payload, .bufsz = 64, .mem = mem_a, .memsz = 0x1000 },
        { .buf = payload, .bufsz = 64, .mem = mem_b, .memsz = 0x1000 },
    };
    struct kexec_segment_wire pair_overlap[2] = {
        { .buf = payload, .bufsz = 64, .mem = mem_a, .memsz = 0x1000 },
        { .buf = payload, .bufsz = 64, .mem = mem_overlap, .memsz = 0x1000 },
    };

    printf("KEXEC: START\n");

    /* Nothing staged yet. */
    expect_reboot("reboot-kexec-before-load", EINVAL);
    expect_load("nr-segments-zero", 0, &one, EINVAL);
    expect_load("nr-segments-over-max", 17, &one, EINVAL);
    expect_load("bufsz-exceeds-memsz", 1, &bad_sizes, EINVAL);

    /* A refused multi-segment load (pairwise overlap) stages nothing. */
    expect_load("pairwise-overlap", 2, pair_overlap, EADDRNOTAVAIL);
    expect_reboot("reboot-kexec-after-refusal", EINVAL);

    /* Valid load: the image is now staged. */
    expect_load("single-segment", 1, &one, 0);
    expect_reboot("reboot-kexec-after-load", ENOSYS);

    /* A later refusal must not disturb the staged image. */
    expect_load("refusal-keeps-image-a", 2, pair_overlap, EADDRNOTAVAIL);
    expect_load("refusal-keeps-image-b", 1, &bad_sizes, EINVAL);
    expect_reboot("reboot-kexec-after-refusals", ENOSYS);

    /* A valid load replaces the image (reset frees the previous frames). */
    expect_load("two-segment-replace", 2, pair_ok, 0);
    expect_reboot("reboot-kexec-after-replace", ENOSYS);

    if (failures) {
        printf("KEXEC: FAIL (%d)\n", failures);
        return 1;
    }
    printf("KEXEC: PASS\n");
    return 0;
}
