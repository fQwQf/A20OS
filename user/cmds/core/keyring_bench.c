/* Fixed-workload Linux keyring syscall microbenchmark.
 *
 * Build the same binary into C- and Rust-selected RV64 Linux guest images.
 * This is optional performance evidence, not a correctness gate. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define KEY_SPEC_SESSION_KEYRING (-3)
#define KEY_SPEC_USER_KEYRING (-4)
#define KEYCTL_GET_KEYRING_ID 0
#define KEYCTL_JOIN_SESSION_KEYRING 1
#define KEYCTL_LINK 8
#define KEYCTL_UNLINK 9
#define KEYCTL_SEARCH 10
#define KEYCTL_READ 11

#ifndef ITERATIONS
#define ITERATIONS 10000
#endif
#ifndef WARMUP
#define WARMUP 1000
#endif

static const char key_desc[] = "p3-keyring-bench-value";
static const char key_payload[] = "fixed-payload";
static long key_serial;
static long session_serial;
static long user_ring_serial;

static void fail(const char *what)
{
    printf("KEYRING_BENCH: FAIL %s errno=%d\n", what, errno);
}

static int64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return -1;
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int add_update_once(void)
{
    long serial = syscall(SYS_add_key, "user", key_desc, key_payload,
                          sizeof(key_payload) - 1, session_serial);
    return serial == key_serial ? 0 : -1;
}

static int search_once(void)
{
    return syscall(SYS_keyctl, KEYCTL_SEARCH, session_serial, "user",
                   key_desc, 0L) == key_serial ? 0 : -1;
}

static int read_once(void)
{
    char value[sizeof(key_payload)] = {0};
    long n = syscall(SYS_keyctl, KEYCTL_READ, key_serial, value,
                     sizeof(value), 0L);
    return n == (long)(sizeof(key_payload) - 1) &&
           memcmp(value, key_payload, sizeof(key_payload) - 1) == 0 ? 0 : -1;
}

static int link_unlink_once(void)
{
    if (syscall(SYS_keyctl, KEYCTL_LINK, key_serial, user_ring_serial,
                0L, 0L) != 0)
        return -1;
    if (syscall(SYS_keyctl, KEYCTL_UNLINK, key_serial, user_ring_serial,
                0L, 0L) != 0)
        return -1;
    return 0;
}

static int warmup(int (*operation)(void))
{
    for (int i = 0; i < WARMUP; ++i) {
        if (operation() < 0)
            return -1;
    }
    return 0;
}

static int64_t measure(int (*operation)(void))
{
    int64_t start = monotonic_ns();
    if (start < 0)
        return -1;
    for (int i = 0; i < ITERATIONS; ++i) {
        if (operation() < 0)
            return -1;
    }
    int64_t end = monotonic_ns();
    return end < start ? -1 : end - start;
}

int main(void)
{
    struct timespec resolution;
    if (clock_getres(CLOCK_MONOTONIC, &resolution) < 0) {
        fail("clock_getres");
        return 1;
    }
    session_serial = syscall(SYS_keyctl, KEYCTL_GET_KEYRING_ID,
                             KEY_SPEC_SESSION_KEYRING, 1L, 0L, 0L);
    if (session_serial <= 0) {
        fail("get session");
        return 1;
    }
    key_serial = syscall(SYS_add_key, "user", key_desc, key_payload,
                         sizeof(key_payload) - 1, session_serial);
    if (key_serial <= 0) {
        fail("create key");
        return 1;
    }
    user_ring_serial = syscall(SYS_keyctl, KEYCTL_GET_KEYRING_ID,
                               KEY_SPEC_USER_KEYRING, 1L, 0L, 0L);
    if (user_ring_serial <= 0) {
        fail("get user ring");
        return 1;
    }

    int (*operations[])(void) = {
        add_update_once, search_once, read_once, link_unlink_once,
    };
    int64_t elapsed[sizeof(operations) / sizeof(operations[0])];
    for (size_t i = 0; i < sizeof(operations) / sizeof(operations[0]); ++i) {
        if (warmup(operations[i]) < 0) {
            fail("warmup operation");
            return 1;
        }
        elapsed[i] = measure(operations[i]);
        if (elapsed[i] < 0) {
            fail("measured operation");
            return 1;
        }
    }

    /* Drop the session-owned key before reporting success. The persistent
     * per-uid ring remains pinned by the subsystem by design. */
    if (syscall(SYS_keyctl, KEYCTL_JOIN_SESSION_KEYRING,
                "p3-keyring-bench-cleanup", 0L, 0L, 0L) <= 0) {
        fail("release session objects");
        return 1;
    }
    printf("KEYRING_BENCH: PASS n=%d warmup=%d clock_res_ns=%lld "
           "add_update_ns=%lld search_ns=%lld read_ns=%lld link_unlink_ns=%lld\n",
           ITERATIONS, WARMUP,
           (long long)resolution.tv_sec * 1000000000LL + resolution.tv_nsec,
           (long long)elapsed[0], (long long)elapsed[1],
           (long long)elapsed[2], (long long)elapsed[3]);
    return 0;
}
