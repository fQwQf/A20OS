#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#define LINUX_REBOOT_MAGIC1    0xfee1deadUL
#define LINUX_REBOOT_MAGIC2    672274793UL
#define LINUX_REBOOT_CMD_KEXEC 0x45584543UL
#define KEXEC_FILE_UNLOAD      1UL

struct kexec_segment_wire {
    const void *buf;
    size_t bufsz;
    const void *mem;
    size_t memsz;
};

static int failures;

static void expect_reboot(const char *what, int want_errno)
{
    errno = 0;
    long rc = syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                      LINUX_REBOOT_CMD_KEXEC);
    if (rc != -1 || errno != want_errno) {
        printf("KEXEC: FAIL %s rc=%ld errno=%d want=%d\n", what, rc, errno, want_errno);
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
            printf("KEXEC: FAIL %s rc=%ld errno=%d want=success\n", what, rc, errno);
            failures++;
        } else {
            printf("KEXEC: ok %s (staged)\n", what);
        }
        return;
    }
    if (rc != -1 || errno != want_errno) {
        printf("KEXEC: FAIL %s rc=%ld errno=%d want=%d\n", what, rc, errno, want_errno);
        failures++;
    } else {
        printf("KEXEC: ok %s (errno=%d)\n", what, errno);
    }
}

static void expect_unload(void)
{
    errno = 0;
    long rc = syscall(SYS_kexec_file_load, -1, -1, 0, NULL, KEXEC_FILE_UNLOAD);
    if (rc != 0) {
        printf("KEXEC: FAIL unload rc=%ld errno=%d\n", rc, errno);
        failures++;
    } else {
        printf("KEXEC: ok unload\n");
    }
}

int main(void)
{
    static unsigned char payload[4096];
    memset(payload, 0xa5, sizeof(payload));
    const void *mem_a = (const void *)0x90000000UL;
    const void *mem_b = (const void *)0x90001000UL;
    const void *mem_overlap = (const void *)0x90000800UL;
    struct kexec_segment_wire one = {
        .buf = payload, .bufsz = sizeof(payload), .mem = mem_a, .memsz = sizeof(payload),
    };
    struct kexec_segment_wire bad_sizes = {
        .buf = payload, .bufsz = sizeof(payload), .mem = mem_a, .memsz = sizeof(payload) / 2,
    };
    struct kexec_segment_wire pair_ok[2] = {
        { payload, 64, mem_a, 0x1000 }, { payload, 64, mem_b, 0x1000 },
    };
    struct kexec_segment_wire pair_overlap[2] = {
        { payload, 64, mem_a, 0x1000 }, { payload, 64, mem_overlap, 0x1000 },
    };

    printf("KEXEC: START\n");
    expect_reboot("reboot-kexec-before-load", EINVAL);
    expect_load("nr-segments-zero", 0, &one, EINVAL);
    expect_load("nr-segments-over-max", 17, &one, EINVAL);
    expect_load("bufsz-exceeds-memsz", 1, &bad_sizes, EINVAL);
    expect_load("refused-pairwise-overlap", 2, pair_overlap, EADDRNOTAVAIL);
    expect_reboot("reboot-kexec-after-refusal", EINVAL);
    expect_load("single-segment", 1, &one, 0);
    expect_load("refusal-keeps-image-a", 2, pair_overlap, EADDRNOTAVAIL);
    expect_load("refusal-keeps-image-b", 1, &bad_sizes, EINVAL);
    expect_load("two-segment-replace", 2, pair_ok, 0);
    expect_unload();
    expect_reboot("reboot-kexec-after-unload", EINVAL);

    if (failures) {
        printf("KEXEC: FAIL (%d)\n", failures);
        return 1;
    }
    printf("KEXEC: PASS\n");
    return 0;
}
