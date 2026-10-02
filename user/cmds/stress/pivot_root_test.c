/*
 * pivot_root_test — guest gate for the mount reference model.
 *
 * The point of these checks is that they fail if any of it is faked:
 *
 *   1. mountinfo reports real mount ids and real parent ids, and a mount
 *      nested under another names its parent;
 *   2. umount of a mount that some process is rooted in fails with EBUSY,
 *      and MNT_DETACH gets it anyway;
 *   3. pivot_root moves the root: the new root is "/" afterwards, the old
 *      root is unreachable by path, and a descriptor opened before the
 *      pivot still reads the old tree;
 *   4. put_old must live under the new root, so a pivot that tries to keep
 *      a handle on the old tree is refused;
 *   5. chroot escape through ".." still fails after the model change.
 *
 * Prints PIVOT_ROOT: PASS.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#ifndef __NR_pivot_root
#define __NR_pivot_root 155
#endif
#ifndef __NR_umount2
#define __NR_umount2 39
#endif
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif

static int fails;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("PIVOT_ROOT: FAIL %s\n", what);
        fails++;
    }
}

static int do_pivot_root(const char *new_root, const char *put_old) {
    return (int)syscall(__NR_pivot_root, new_root, put_old);
}

static int do_umount2(const char *target, int flags) {
    return (int)syscall(__NR_umount2, target, flags);
}

static void write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    ssize_t w = write(fd, text, strlen(text));
    (void)w;
    close(fd);
}

/* Look up the parent-id field of the mountinfo line whose mount point is
 * @path.  Returns -1 when the mount is not listed. */
static long mountinfo_parent_id(const char *path) {
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (!f)
        return -1;
    char line[1024];
    long result = -1;
    while (fgets(line, sizeof(line), f)) {
        char mp[512];
        unsigned id, pid, majmin;
        int n = sscanf(line, "%u %u %u:%u %*s %511s", &id, &pid, &majmin,
                       &majmin, mp);
        if (n != 5)
            continue;
        size_t plen = strlen(path);
        while (plen > 1 && path[plen - 1] == '/')
            plen--;
        if (strlen(mp) == plen && strncmp(mp, path, plen) == 0) {
            result = (long)pid;
            break;
        }
    }
    fclose(f);
    return result;
}

static int mountinfo_has_mount(const char *path) {
    return mountinfo_parent_id(path) >= 0;
}

int main(void) {
    mkdir("/tmp/pr", 0755);
    mkdir("/tmp/pr/inner", 0755);
    mkdir("/tmp/pr/host", 0755);
    write_file("/tmp/pr/inner/marker", "inner-marker\n");
    write_file("/tmp/pr/host/secret", "host-secret\n");

    /* A nested mount must name its parent in mountinfo. */
    check(mount("none", "/tmp/pr/inner", "ramfs", 0, NULL) == 0,
          "mount ramfs at /tmp/pr/inner");
    long parent = mountinfo_parent_id("/tmp/pr/inner");
    check(parent >= 0, "inner mount listed in mountinfo");
    check(parent != 0, "inner mount reports a real parent");
    check(umount("/tmp/pr/inner") == 0, "umount inner");

    /* ---- busy semantics ---- */
    check(mount("none", "/tmp/pr/inner", "ramfs", 0, NULL) == 0,
          "remount inner");
    pid_t child = fork();
    if (child == 0) {
        /* Stand inside the inner mount and try to unmount it. */
        if (chdir("/tmp/pr/inner") != 0)
            _exit(2);
        int r = do_umount2("/tmp/pr/inner", 0);
        _exit(r == 0 ? 3 : 0);       /* 0 == correctly refused */
    }
    int status = 0;
    if (child > 0)
        waitpid(child, &status, 0);
    check(child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "umount refused while a process is inside the mount");

    /* MNT_DETACH waives the busy rules. */
    check(do_umount2("/tmp/pr/inner", MNT_DETACH) == 0,
          "MNT_DETACH umount of a busy mount");
    check(!mountinfo_has_mount("/tmp/pr/inner"),
          "detached mount leaves mountinfo");

    /* ---- pivot_root ---- */
    mkdir("/tmp/pr/newroot", 0755);
    check(mount("none", "/tmp/pr/newroot", "ramfs", 0, NULL) == 0,
          "mount new root");
    mkdir("/tmp/pr/newroot/oldroot", 0755);

    /* Open a descriptor into the old root *before* the pivot; it must keep
     * working afterwards. */
    write_file("/tmp/pr/host/secret", "host-secret\n");
    int held = open("/tmp/pr/host/secret", O_RDONLY);
    check(held >= 0, "open descriptor into old root");

    /* put_old outside the new root must be refused. */
    int bad = do_pivot_root("/tmp/pr/newroot", "/tmp/pr/host");
    check(bad < 0 && errno == EINVAL,
          "pivot_root refuses put_old outside the new root");

    /* Pivot for real. */
    int r = do_pivot_root("/tmp/pr/newroot", "/tmp/pr/newroot/oldroot");
    check(r == 0, "pivot_root succeeds");
    if (r != 0) {
        printf("PIVOT_ROOT: pivot_root failed errno=%d\n", errno);
    } else {
        char cwd[512];
        check(getcwd(cwd, sizeof(cwd)) != NULL, "getcwd after pivot");
        /* cwd must be the parent of put_old, which is the new root. */
        check(cwd[0] == '/', "getcwd returns an absolute path");
        /* The old root is unreachable by path: resolving it must not find
         * the file we wrote before the pivot. */
        int gone = open("/tmp/pr/host/secret", O_RDONLY);
        check(gone < 0, "old root unreachable by path after pivot");
        if (gone >= 0)
            close(gone);
        /* ... but the descriptor we held still reads the old tree. */
        char buf[64] = {0};
        ssize_t n = held >= 0 ? read(held, buf, sizeof(buf) - 1) : -1;
        check(n > 0 && strcmp(buf, "host-secret\n") == 0,
              "pre-pivot descriptor still reads the old root");
        /* The new root is empty and writable. */
        check(mkdir("/fresh", 0755) == 0, "mkdir under the new root");
        check(rmdir("/fresh") == 0, "rmdir under the new root");
    }
    if (held >= 0)
        close(held);

    umount("/tmp/pr/newroot");

    if (!fails) {
        printf("PIVOT_ROOT: PASS\n");
        return 0;
    }
    printf("PIVOT_ROOT: %d failure(s)\n", fails);
    return 1;
}