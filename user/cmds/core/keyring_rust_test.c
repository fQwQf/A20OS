/* Linux keyring syscall contract test for the Rust-selected kernel profile. */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define KEY_SPEC_THREAD_KEYRING (-1)
#define KEY_SPEC_PROCESS_KEYRING (-2)
#define KEY_SPEC_SESSION_KEYRING (-3)
#define KEY_SPEC_USER_KEYRING (-4)
#define KEYCTL_GET_KEYRING_ID 0
#define KEYCTL_JOIN_SESSION_KEYRING 1
#define KEYCTL_CHOWN 4
#define KEYCTL_SETPERM 5
#define KEYCTL_DESCRIBE 6
#define KEYCTL_CLEAR 7
#define KEYCTL_LINK 8
#define KEYCTL_UNLINK 9
#define KEYCTL_SEARCH 10
#define KEYCTL_READ 11
#define KEYCTL_SET_TIMEOUT 15
#define KEYCTL_INVALIDATE 20

#define KEY_POS_ALL 0x3f000000U
#define KEY_USR_ALL 0x003f0000U

#define CHECK(cond, label) do { \
    if (!(cond)) { \
        printf("KEYRING_RUST: FAIL %s errno=%d\n", label, errno); \
        return 1; \
    } \
} while (0)

static int expect_errno(long result, int expected)
{
    return result == -1 && errno == expected;
}

int main(void)
{
    long session = syscall(SYS_keyctl, KEYCTL_GET_KEYRING_ID,
                           KEY_SPEC_SESSION_KEYRING, 1L, 0L, 0L);
    CHECK(session > 0, "get session ring");

    static const char payload[] = "rust-owned-payload";
    long key = syscall(SYS_add_key, "user", "p3-rust-key", payload,
                       sizeof(payload) - 1, session);
    CHECK(key > 0, "add key");
    long duplicate = syscall(SYS_add_key, "user", "p3-rust-key", "updated",
                             (size_t)7, session);
    CHECK(duplicate == key, "replace retains serial");

    char value[32] = {0};
    long n = syscall(SYS_keyctl, KEYCTL_READ, key, value, sizeof(value), 0L);
    CHECK(n == 7 && memcmp(value, "updated", 7) == 0, "read payload");

    char desc[128] = {0};
    long desc_size = syscall(SYS_keyctl, KEYCTL_DESCRIBE, key, desc,
                             sizeof(desc), 0L);
    CHECK(desc_size > 1 && memmem(desc, (size_t)desc_size, "user;", 5) != NULL &&
          memmem(desc, (size_t)desc_size, ";p3-rust-key", 12) != NULL,
          "describe data");
    long desc_need = syscall(SYS_keyctl, KEYCTL_DESCRIBE, key, 0L, 0L, 0L);
    CHECK(desc_need >= desc_size, "describe length query compatibility");

    long found = syscall(SYS_keyctl, KEYCTL_SEARCH, session, "user",
                         "p3-rust-key", 0L);
    CHECK(found == key, "direct search");
    long user_ring = syscall(SYS_keyctl, KEYCTL_GET_KEYRING_ID,
                             KEY_SPEC_USER_KEYRING, 1L, 0L, 0L);
    CHECK(user_ring > 0, "get user ring");
    CHECK(syscall(SYS_keyctl, KEYCTL_LINK, session, user_ring, 0L, 0L) == 0,
          "link nested session ring");
    found = syscall(SYS_keyctl, KEYCTL_SEARCH, user_ring, "user",
                    "p3-rust-key", 0L);
    CHECK(found == key, "nested search");
    CHECK(syscall(SYS_keyctl, KEYCTL_UNLINK, session, user_ring, 0L, 0L) == 0,
          "unlink nested session ring");
    CHECK(syscall(SYS_keyctl, KEYCTL_UNLINK, session, user_ring, 0L, 0L) == -1 &&
          errno == ENOENT, "unlink missing relation");

    CHECK(syscall(SYS_keyctl, KEYCTL_SET_TIMEOUT, key, ULONG_MAX, 0L, 0L) == 0,
          "set saturating huge timeout");
    CHECK(syscall(SYS_keyctl, KEYCTL_READ, key, value, sizeof(value), 0L) == 7,
          "huge timeout read does not expire");
    CHECK(syscall(SYS_keyctl, KEYCTL_SEARCH, session, "user", "p3-rust-key", 0L) == key,
          "huge timeout search does not expire");
    CHECK(syscall(SYS_keyctl, KEYCTL_SET_TIMEOUT, key, 0L, 0L, 0L) == 0,
          "zero timeout clears expiry");

    /* The test init runs as root. A forked, unprivileged child must not be
     * able to change an object owned by the parent. */
    CHECK(geteuid() == 0, "root test fixture for permission denial");
    pid_t child = fork();
    CHECK(child >= 0, "fork permission case");
    if (child == 0) {
        if (setuid(65534) < 0)
            _exit(31);
        long r = syscall(SYS_keyctl, KEYCTL_SETPERM, key, 0L, 0L, 0L);
        _exit(expect_errno(r, EPERM) ? 0 : 32);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0, "owner permission denied");

    /* The stored permission mask constrains read/search/link.  The owner can
     * still change the mask (SETPERM needs ownership, not the bits it is
     * replacing), which is what lets this block restore them. */
    long perm_key = syscall(SYS_add_key, "user", "p3-perm-key", "secret",
                            (size_t)6, session);
    CHECK(perm_key > 0, "add permission key");
    CHECK(syscall(SYS_keyctl, KEYCTL_SETPERM, perm_key, 0L, 0L, 0L) == 0,
          "clear key permissions");
    errno = 0;
    CHECK(syscall(SYS_keyctl, KEYCTL_READ, perm_key, value, sizeof(value), 0L) == -1 &&
          errno == EACCES, "read denied by permission mask");
    errno = 0;
    CHECK(syscall(SYS_keyctl, KEYCTL_DESCRIBE, perm_key, desc, sizeof(desc), 0L) == -1 &&
          errno == EACCES, "describe denied by permission mask");
    errno = 0;
    CHECK(syscall(SYS_keyctl, KEYCTL_SEARCH, session, "user", "p3-perm-key", 0L) == -1 &&
          errno == ENOKEY, "search skips unsearchable key");
    errno = 0;
    CHECK(syscall(SYS_keyctl, KEYCTL_LINK, perm_key, user_ring, 0L, 0L) == -1 &&
          errno == EACCES, "link denied by permission mask");
    errno = 0;
    CHECK(syscall(SYS_keyctl, KEYCTL_INVALIDATE, perm_key, 0L, 0L, 0L) == -1 &&
          errno == EPERM, "invalidate denied by permission mask");
    CHECK(syscall(SYS_keyctl, KEYCTL_SETPERM, perm_key,
                  KEY_POS_ALL | KEY_USR_ALL, 0L, 0L) == 0,
          "restore key permissions");
    CHECK(syscall(SYS_keyctl, KEYCTL_READ, perm_key, value, sizeof(value), 0L) == 6 &&
          memcmp(value, "secret", 6) == 0, "read after permission restore");

    /* INVALIDATE expires and detaches the key from every ring, so both the
     * serial and the name stop resolving. */
    CHECK(syscall(SYS_keyctl, KEYCTL_INVALIDATE, perm_key, 0L, 0L, 0L) == 0,
          "invalidate key");
    errno = 0;
    CHECK(syscall(SYS_keyctl, KEYCTL_READ, perm_key, value, sizeof(value), 0L) == -1 &&
          errno == ENOKEY, "invalidated serial is gone");
    errno = 0;
    CHECK(syscall(SYS_keyctl, KEYCTL_SEARCH, session, "user", "p3-perm-key", 0L) == -1 &&
          errno == ENOKEY, "invalidated key is unsearchable");

    CHECK(syscall(SYS_keyctl, KEYCTL_SET_TIMEOUT, key, 1L, 0L, 0L) == 0,
          "set timeout");
    struct timespec delay = { .tv_sec = 1, .tv_nsec = 200000000L };
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
    CHECK(syscall(SYS_keyctl, KEYCTL_READ, key, value, sizeof(value), 0L) == -1 &&
          errno == EKEYEXPIRED, "expired read");

    /* The old session ring stays indexed while member rings retain it. A
     * direct cycle and a three-ring cycle are both rejected. The accepted
     * diamond has two owners of the old ring and tears down only on the last
     * unlink. */
    CHECK(syscall(SYS_keyctl, KEYCTL_LINK, session, user_ring, 0L, 0L) == 0,
          "cycle fixture user owns session");
    CHECK(syscall(SYS_keyctl, KEYCTL_LINK, user_ring, session, 0L, 0L) == -1 &&
          errno == EDEADLK, "reject two-ring cycle");
    long ring_c = syscall(SYS_keyctl, KEYCTL_JOIN_SESSION_KEYRING,
                          "p3-cycle-c", 0L, 0L, 0L);
    CHECK(ring_c > 0, "create cycle ring c");
    long dag_key = syscall(SYS_add_key, "user", "p3-dag-key", "dag", (size_t)3,
                           session);
    CHECK(dag_key > 0, "add retained ring key");
    CHECK(syscall(SYS_keyctl, KEYCTL_LINK, session, ring_c, 0L, 0L) == 0,
          "second ring owns old session");
    CHECK(syscall(SYS_keyctl, KEYCTL_UNLINK, session, user_ring, 0L, 0L) == 0,
          "drop first old-session owner");
    CHECK(syscall(SYS_keyctl, KEYCTL_LINK, ring_c, user_ring, 0L, 0L) == 0,
          "user ring owns ring c");
    CHECK(syscall(SYS_keyctl, KEYCTL_LINK, user_ring, session, 0L, 0L) == -1 &&
          errno == EDEADLK, "reject three-ring cycle");
    CHECK(syscall(SYS_keyctl, KEYCTL_LINK, session, user_ring, 0L, 0L) == 0,
          "second parent owns old session");
    CHECK(syscall(SYS_keyctl, KEYCTL_UNLINK, session, user_ring, 0L, 0L) == 0,
          "drop one diamond parent");
    CHECK(syscall(SYS_keyctl, KEYCTL_READ, session, value, sizeof(value), 0L) > 0,
          "old ring survives first unlink");
    CHECK(syscall(SYS_keyctl, KEYCTL_UNLINK, session, ring_c, 0L, 0L) == 0,
          "drop last old-session parent");
    CHECK(syscall(SYS_keyctl, KEYCTL_READ, dag_key, value, sizeof(value), 0L) == -1 &&
          errno == ENOKEY, "last unlink reclaims old key");
    CHECK(syscall(SYS_keyctl, KEYCTL_GET_KEYRING_ID, session, 0L, 0L, 0L) == -1 &&
          errno == ENOKEY, "last unlink removes ring serial");
    long ring_d = syscall(SYS_keyctl, KEYCTL_JOIN_SESSION_KEYRING,
                          "p3-cycle-d", 0L, 0L, 0L);
    CHECK(ring_d > 0, "replace session after ring c");
    CHECK(syscall(SYS_keyctl, KEYCTL_UNLINK, ring_c, user_ring, 0L, 0L) == 0,
          "release ring c from user ring");
    CHECK(syscall(SYS_keyctl, KEYCTL_GET_KEYRING_ID, ring_c, 0L, 0L, 0L) == -1 &&
          errno == ENOKEY, "ring c released");

    char too_long_type[40];
    memset(too_long_type, 't', sizeof(too_long_type));
    too_long_type[sizeof(too_long_type) - 1] = '\0';
    long live_session = syscall(SYS_keyctl, KEYCTL_GET_KEYRING_ID,
                                KEY_SPEC_SESSION_KEYRING, 0L, 0L, 0L);
    CHECK(live_session > 0, "get live session for input boundaries");
    /* user_strncpy's established ABI truncates to max-1 and terminates.
     * Keep this distinct from bad-pointer rejection and assert the truncated
     * key type can be searched under the existing helper contract. */
    long truncated_key = syscall(SYS_add_key, too_long_type, "bad-type", "x",
                                 (size_t)1, live_session);
    CHECK(truncated_key > 0, "overlong type is bounded and truncated");
    char truncated_type[32];
    memset(truncated_type, 't', sizeof(truncated_type) - 1);
    truncated_type[sizeof(truncated_type) - 1] = '\0';
    CHECK(syscall(SYS_keyctl, KEYCTL_SEARCH, live_session, truncated_type,
                  "bad-type", 0L) == truncated_key,
          "search type truncated by usercopy helper");
    CHECK(syscall(SYS_add_key, (const char *)(uintptr_t)1, "bad-type", "x",
                  (size_t)1, live_session) == -1 && errno == EFAULT,
          "invalid type pointer");
    CHECK(syscall(SYS_add_key, "user", "null-payload", NULL, (size_t)1,
                  live_session) == -1 && errno == EFAULT, "null payload");
    static char oversized[64 * 1024 + 1];
    CHECK(syscall(SYS_add_key, "user", "oversized", oversized,
                  sizeof(oversized), live_session) == -1 && errno == E2BIG,
          "payload limit");
    CHECK(syscall(SYS_add_key, "user", "missing-ring", "x", (size_t)1,
                  0x7fffffff) == -1 && errno == ENOKEY,
          "unknown destination ring");

    /* Replacing an unshared session ring drops its serial-map objects. */
    CHECK(syscall(SYS_add_key, "user", "cleanup-key", "c", (size_t)1,
                  ring_d) > 0, "add cleanup key");
    CHECK(syscall(SYS_keyctl, KEYCTL_JOIN_SESSION_KEYRING, "p3-new-session",
                  0L, 0L, 0L) > 0, "replace session");
    CHECK(syscall(SYS_keyctl, KEYCTL_READ, key, value, sizeof(value), 0L) == -1 &&
          errno == ENOKEY, "old key resource released");

    /* A real fork holds the old session alive while the parent installs a
     * replacement. The child must still resolve/search through its inherited
     * session slot; after its exit the final old serial must disappear. */
    long inherited = syscall(SYS_keyctl, KEYCTL_GET_KEYRING_ID,
                             KEY_SPEC_SESSION_KEYRING, 1L, 0L, 0L);
    CHECK(inherited > 0, "get fork session");
    long inherited_key = syscall(SYS_add_key, "user", "fork-held-key", "f",
                                 (size_t)1, inherited);
    CHECK(inherited_key > 0, "add fork-held key");
    int gate[2];
    CHECK(pipe(gate) == 0, "create fork gate");
    pid_t inheriting_child = fork();
    CHECK(inheriting_child >= 0, "fork session lifetime child");
    if (inheriting_child == 0) {
        close(gate[1]);
        char byte;
        if (read(gate[0], &byte, 1) != 1)
            _exit(41);
        long child_ring = syscall(SYS_keyctl, KEYCTL_GET_KEYRING_ID,
                                  KEY_SPEC_SESSION_KEYRING, 0L, 0L, 0L);
        long child_key = syscall(SYS_keyctl, KEYCTL_SEARCH,
                                 KEY_SPEC_SESSION_KEYRING, "user",
                                 "fork-held-key", 0L);
        _exit(child_ring == inherited && child_key == inherited_key ? 0 : 42);
    }
    close(gate[0]);
    CHECK(syscall(SYS_keyctl, KEYCTL_JOIN_SESSION_KEYRING,
                  "p3-parent-after-fork", 0L, 0L, 0L) > 0,
          "parent replaces inherited session");
    CHECK(syscall(SYS_keyctl, KEYCTL_GET_KEYRING_ID, inherited,
                  0L, 0L, 0L) == inherited,
          "child reference keeps old serial indexed");
    CHECK(write(gate[1], "x", 1) == 1, "release fork child");
    close(gate[1]);
    CHECK(waitpid(inheriting_child, &status, 0) == inheriting_child &&
          WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "child searches inherited session after parent join");
    CHECK(syscall(SYS_keyctl, KEYCTL_GET_KEYRING_ID, inherited,
                  0L, 0L, 0L) == -1 && errno == ENOKEY,
          "final child release removes old serial");

    /* CLEAR empties one ring and needs WRITE on it.  This is the last case:
     * joining another session ring replaces the task's current slot. */
    long clear_ring = syscall(SYS_keyctl, KEYCTL_JOIN_SESSION_KEYRING,
                              "p3-clear-ring", 0L, 0L, 0L);
    CHECK(clear_ring > 0, "create clear ring");
    CHECK(syscall(SYS_add_key, "user", "p3-clear-a", "a", (size_t)1,
                  clear_ring) > 0, "add clear key a");
    CHECK(syscall(SYS_add_key, "user", "p3-clear-b", "b", (size_t)1,
                  clear_ring) > 0, "add clear key b");
    CHECK(syscall(SYS_keyctl, KEYCTL_SETPERM, clear_ring, 0L, 0L, 0L) == 0,
          "clear ring loses write");
    errno = 0;
    CHECK(syscall(SYS_keyctl, KEYCTL_CLEAR, clear_ring, 0L, 0L, 0L) == -1 &&
          errno == EACCES, "clear denied by permission mask");
    CHECK(syscall(SYS_keyctl, KEYCTL_SETPERM, clear_ring,
                  KEY_POS_ALL | KEY_USR_ALL, 0L, 0L) == 0,
          "restore clear ring permissions");
    CHECK(syscall(SYS_keyctl, KEYCTL_CLEAR, clear_ring, 0L, 0L, 0L) == 0,
          "clear ring");
    CHECK(syscall(SYS_keyctl, KEYCTL_READ, clear_ring, value, sizeof(value), 0L) == 0,
          "cleared ring is empty");
    errno = 0;
    CHECK(syscall(SYS_keyctl, KEYCTL_SEARCH, clear_ring, "user", "p3-clear-a",
                  0L) == -1 && errno == ENOKEY, "cleared key is gone");

    printf("KEYRING_RUST: PASS session=%ld key=%ld desc=%ld query=%ld\n",
           session, key, desc_size, desc_need);
    return 0;
}
