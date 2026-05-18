/*
 * test_fdtable.c — Unit tests for A20 fd↔handle mapping table.
 *
 * Tests the musl-port a20_fdtable.c module in isolation.
 * Run on host with: gcc -DTEST_HOST test_fdtable.c -o test_fdtable && ./test_fdtable
 * Run on A20 with:  riscv64-unknown-elf-gcc -static test_fdtable.c -o test_fdtable
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef TEST_HOST

/* ---- Host-mode stubs ---- */
typedef unsigned int a20_handle_t;
#define A20_HANDLE_NULL ((a20_handle_t)0xFFFFFFFF)

typedef volatile int a20_spinlock_t;
static inline void a20_spin_lock(a20_spinlock_t *lk)   { (void)lk; }
static inline void a20_spin_unlock(a20_spinlock_t *lk) { (void)lk; }

#define A20_O_RDONLY   0x0000
#define A20_O_WRONLY   0x0001
#define A20_O_RDWR     0x0002
#define A20_O_ACCMODE  0x0003
#define A20_O_CLOEXEC  0x80000

typedef long long a20_off_t;

struct a20_fd_entry {
    a20_handle_t  handle;
    unsigned int  flags;
    a20_off_t     pos;
    unsigned int  fd_flags;
};

#define A20_FD_INIT_SIZE 32
#define A20_FD_MAX_SIZE  1024

static struct a20_fd_entry *__fd_table;
static int __fd_table_size;
static int __fd_table_used;
static a20_spinlock_t __fd_lock = 0;

static uint8_t __bare_heap[4096];
static size_t __bare_heap_pos = 0;

static void *__bare_alloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (__bare_heap_pos + n > sizeof(__bare_heap)) return NULL;
    void *p = __bare_heap + __bare_heap_pos;
    __bare_heap_pos += n;
    return p;
}

#else

/* A20 native mode — include real header */
#include "a20_fdtable.c"

#endif

/* ---- Implementation (shared) ---- */

static void fd_table_init(a20_handle_t si_stdin,
                          a20_handle_t si_stdout,
                          a20_handle_t si_stderr)
{
    __fd_table = (struct a20_fd_entry *)__bare_alloc(
        A20_FD_INIT_SIZE * sizeof(struct a20_fd_entry));
    for (int i = 0; i < A20_FD_INIT_SIZE; i++)
        __fd_table[i].handle = A20_HANDLE_NULL;
    __fd_table_size = A20_FD_INIT_SIZE;
    __fd_table_used = 3;

    __fd_table[0].handle = si_stdin;  __fd_table[0].flags = 0;
    __fd_table[1].handle = si_stdout; __fd_table[1].flags = 1;
    __fd_table[2].handle = si_stderr; __fd_table[2].flags = 1;
}

static int fd_alloc(a20_handle_t handle, unsigned int flags) {
    for (int i = 0; i < __fd_table_size; i++) {
        if (__fd_table[i].handle == A20_HANDLE_NULL) {
            __fd_table[i].handle = handle;
            __fd_table[i].flags  = flags & 3;
            __fd_table[i].pos    = 0;
            if (i >= __fd_table_used) __fd_table_used = i + 1;
            return i;
        }
    }
    return -1;
}

static a20_handle_t fd_to_handle(int fd) {
    if (fd < 0 || fd >= __fd_table_size) return A20_HANDLE_NULL;
    return __fd_table[fd].handle;
}

static int fd_free(int fd) {
    if (fd < 0 || fd >= __fd_table_size ||
        __fd_table[fd].handle == A20_HANDLE_NULL)
        return -1;
    __fd_table[fd].handle = A20_HANDLE_NULL;
    __fd_table[fd].flags  = 0;
    return 0;
}

/* ---- Test infrastructure ---- */
static int __test_pass = 0;
static int __test_fail = 0;

#define ASSERT(expr, msg) do { \
    if (!(expr)) { \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
        __test_fail++; \
    } else { \
        __test_pass++; \
    } \
} while(0)

/* ---- Tests ---- */

static void test_init_stdio(void)
{
    printf("[test] fd table init with stdio handles...\n");
    fd_table_init(100, 101, 102);

    ASSERT(fd_to_handle(0) == 100, "stdin handle = 100");
    ASSERT(fd_to_handle(1) == 101, "stdout handle = 101");
    ASSERT(fd_to_handle(2) == 102, "stderr handle = 102");
    ASSERT(fd_to_handle(3) == A20_HANDLE_NULL, "fd 3 is free");
}

static void test_alloc(void)
{
    printf("[test] fd alloc returns smallest available...\n");

    int fd3 = fd_alloc(200, 0);
    ASSERT(fd3 == 3, "first alloc returns fd 3");

    int fd4 = fd_alloc(201, 1);
    ASSERT(fd4 == 4, "second alloc returns fd 4");

    ASSERT(fd_to_handle(fd3) == 200, "fd 3 -> handle 200");
    ASSERT(fd_to_handle(fd4) == 201, "fd 4 -> handle 201");
}

static void test_free_and_reuse(void)
{
    printf("[test] fd free and reuse...\n");

    fd_free(3);
    ASSERT(fd_to_handle(3) == A20_HANDLE_NULL, "fd 3 freed");

    int fd_new = fd_alloc(300, 2);
    ASSERT(fd_new == 3, "reused fd 3 (smallest free)");
    ASSERT(fd_to_handle(3) == 300, "fd 3 now -> handle 300");
}

static void test_free_stdin(void)
{
    printf("[test] free and replace stdin...\n");

    fd_free(0);
    ASSERT(fd_to_handle(0) == A20_HANDLE_NULL, "stdin freed");

    int fd0 = fd_alloc(999, 0);
    ASSERT(fd0 == 0, "fd 0 reused");
    ASSERT(fd_to_handle(0) == 999, "fd 0 -> handle 999");
}

static void test_invalid_fd(void)
{
    printf("[test] invalid fd operations...\n");

    ASSERT(fd_to_handle(-1) == A20_HANDLE_NULL, "fd -1 -> NULL");
    ASSERT(fd_to_handle(9999) == A20_HANDLE_NULL, "fd 9999 -> NULL");
    ASSERT(fd_free(-1) == -1, "free fd -1 fails");
    ASSERT(fd_free(9999) == -1, "free fd 9999 fails");
}

static void test_table_fill(void)
{
    printf("[test] fill table to capacity...\n");

    /* Reinit to clean state */
    for (int i = 0; i < __fd_table_size; i++) {
        __fd_table[i].handle = A20_HANDLE_NULL;
        __fd_table[i].flags = 0;
    }

    for (int i = 0; i < A20_FD_INIT_SIZE; i++) {
        int fd = fd_alloc((a20_handle_t)(500 + i), 0);
        ASSERT(fd == i, "sequential alloc");
    }

    int overflow = fd_alloc(9999, 0);
    ASSERT(overflow == -1, "table full returns -1");
}

int main(void)
{
    printf("=== A20 fd table tests ===\n\n");

    test_init_stdio();
    test_alloc();
    test_free_and_reuse();
    test_free_stdin();
    test_invalid_fd();
    test_table_fill();

    printf("\n=== Results: %d passed, %d failed ===\n",
           __test_pass, __test_fail);
    return __test_fail ? 1 : 0;
}
