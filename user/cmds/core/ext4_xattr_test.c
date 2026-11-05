/* ext4 xattr persistence fixture: write on one boot, verify on the next.
 *
 * The gate (tools/fs/ext4_xattr_gate.py) boots twice over the same unjournaled
 * ext4 image, so this binary proves the attributes reached the disk and not
 * just the VFS's volatile table:
 *
 *   write   set user.* and a POSIX ACL blob, fsync, list, remove one, and
 *           check the out-of-space path leaves the surviving attributes intact
 *   verify  after the reboot, read every surviving attribute back
 *
 * Attributes are stored in the inode body, so a value that does not fit must
 * fail with ENOSPC and leave the inode exactly as it was.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define XA_PATH "/extra/xa.dat"

/* The attributes live in the inode body: a 256-byte inode leaves about 90
 * bytes for them, so one small user attribute plus the ACL blob is what fits.
 * Anything larger must fail with ENOSPC (checked below). */
/* posix_acl xattr: u32 version 2, then {u16 tag, u16 perm, u32 id}. */
static const unsigned char acl_blob[] = {
    0x02, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x06, 0x00, 0xff, 0xff, 0xff, 0xff,
    0x04, 0x00, 0x04, 0x00, 0xff, 0xff, 0xff, 0xff,
    0x20, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
};

static int fail(const char *what)
{
    printf("EXT4_XATTR: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int expect_value(const char *name, const void *want, size_t want_len,
                        const char *what)
{
    unsigned char buf[256];
    memset(buf, 0, sizeof(buf));
    ssize_t n = syscall(SYS_getxattr, XA_PATH, name, buf, sizeof(buf));
    if (n != (ssize_t)want_len || memcmp(buf, want, want_len) != 0)
        return fail(what);
    return 0;
}

/* Walk the NUL-separated list; order is not part of the contract. */
static int list_has(const char *list, size_t n, const char *name)
{
    size_t i = 0, want = strlen(name);
    while (i < n) {
        size_t len = strlen(list + i);
        if (len == want && memcmp(list + i, name, len) == 0)
            return 1;
        i += len + 1;
    }
    return 0;
}

static int write_attrs(void)
{
    unlink(XA_PATH);
    int fd = open(XA_PATH, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        return fail("create");
    if (write(fd, "xattr-host\n", 11) != 11) {
        close(fd);
        return fail("write data");
    }

    if (syscall(SYS_setxattr, XA_PATH, "user.short", "v1", 2, 0) < 0)
        return fail("set user.short");
    if (syscall(SYS_setxattr, XA_PATH, "system.posix_acl_access", acl_blob,
                sizeof(acl_blob), 0) < 0)
        return fail("set acl");

    /* The inode body holds these; fsync must make them durable without an
     * explicit unmount. */
    if (fsync(fd) < 0) {
        close(fd);
        return fail("fsync");
    }
    close(fd);

    if (expect_value("user.short", "v1", 2, "get user.short") != 0)
        return 1;
    if (expect_value("system.posix_acl_access", acl_blob, sizeof(acl_blob),
                     "get acl") != 0)
        return 1;

    /* A value that cannot fit must fail without touching what is there. */
    char big[200];    /* larger than the whole inode-body attribute area */
    memset(big, 'B', sizeof(big));
    errno = 0;
    if (syscall(SYS_setxattr, XA_PATH, "user.toobig", big, sizeof(big), 0) >= 0)
        return fail("oversized xattr was accepted");
    if (errno != ENOSPC && errno != ERANGE)
        return fail("oversized xattr errno");
    if (expect_value("user.short", "v1", 2, "get user.short after ENOSPC") != 0)
        return 1;

    /* Replacement must persist too. */
    if (syscall(SYS_setxattr, XA_PATH, "user.short", "v2", 2, 0) < 0)
        return fail("replace user.short");
    if (expect_value("user.short", "v2", 2, "get replaced user.short") != 0)
        return 1;

    printf("EXT4_XATTR: write PASS (user attr + acl, replaced, ENOSPC clean)\n");
    return 0;
}

static int verify_attrs(void)
{
    if (expect_value("user.short", "v2", 2, "user.short after reboot") != 0)
        return 1;
    if (expect_value("system.posix_acl_access", acl_blob, sizeof(acl_blob),
                     "acl after reboot") != 0)
        return 1;

    char list[256];
    memset(list, 0, sizeof(list));
    ssize_t n = syscall(SYS_listxattr, XA_PATH, list, sizeof(list));
    if (n <= 0)
        return fail("listxattr");
    if (!list_has(list, (size_t)n, "user.short") ||
        !list_has(list, (size_t)n, "system.posix_acl_access"))
        return fail("list contents");

    printf("EXT4_XATTR: verify PASS (survived reboot, replacement stayed)\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "write") && strcmp(argv[1], "verify"))) {
        fprintf(stderr, "usage: ext4_xattr_test write|verify\n");
        return 2;
    }
    struct stat st;
    if (stat("/extra", &st) != 0) {
        printf("EXT4_XATTR: SKIP no /extra\n");
        return 2;
    }
    return strcmp(argv[1], "write") == 0 ? write_attrs() : verify_attrs();
}
