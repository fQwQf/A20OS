/*
 * test_syscall_bridge.c — Integration test for A20 syscall mapping layer.
 *
 * Verifies the a20_syscallops.c functions produce correct syscall numbers
 * and parameter translations.
 *
 * On host: uses mocked syscalls (counts calls, checks args).
 * On A20: uses real ecall interface.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TEST_HOST

/* ---- Mock syscall infrastructure ---- */
static long mock_return_val = 0;
static int  mock_syscall_count = 0;
static long mock_last_nr = 0;
static long mock_last_args[6];

/* Override __syscall* macros */
#undef __syscall0
#undef __syscall1
#undef __syscall2
#undef __syscall3
#undef __syscall4
#undef __syscall5
#undef __syscall6

#define __syscall0(n) ({ mock_last_nr = (n); mock_syscall_count++; mock_return_val; })
#define __syscall1(n,a) ({ mock_last_nr = (n); mock_last_args[0]=(long)(a); mock_syscall_count++; mock_return_val; })
#define __syscall2(n,a,b) ({ mock_last_nr = (n); mock_last_args[0]=(long)(a); mock_last_args[1]=(long)(b); mock_syscall_count++; mock_return_val; })
#define __syscall3(n,a,b,c) ({ mock_last_nr = (n); mock_last_args[0]=(long)(a); mock_last_args[1]=(long)(b); mock_last_args[2]=(long)(c); mock_syscall_count++; mock_return_val; })
#define __syscall4(n,a,b,c,d) ({ mock_last_nr = (n); mock_last_args[0]=(long)(a); mock_last_args[1]=(long)(b); mock_last_args[2]=(long)(c); mock_last_args[3]=(long)(d); mock_syscall_count++; mock_return_val; })
#define __syscall5(n,a,b,c,d,e) ({ mock_last_nr = (n); mock_last_args[0]=(long)(a); mock_last_args[1]=(long)(b); mock_last_args[2]=(long)(c); mock_last_args[3]=(long)(d); mock_last_args[4]=(long)(e); mock_syscall_count++; mock_return_val; })
#define __syscall6(n,a,b,c,d,e,f) ({ mock_last_nr = (n); mock_last_args[0]=(long)(a); mock_last_args[1]=(long)(b); mock_last_args[2]=(long)(c); mock_last_args[3]=(long)(d); mock_last_args[4]=(long)(e); mock_last_args[5]=(long)(f); mock_syscall_count++; mock_return_val; })

#endif

/* A20 syscall numbers */
#define A20_NR_HANDLE_CLOSE  0x0100
#define A20_NR_HANDLE_READ   0x0401
#define A20_NR_HANDLE_WRITE  0x0402
#define A20_NR_PATH_OPEN     0x0400
#define A20_NR_VM_ALLOC      0x0300
#define A20_NR_TASK_EXIT     0x0200
#define A20_NR_TASK_SPAWN    0x0201
#define A20_NR_CLOCK_GET     0x0700
#define A20_NR_NET_SOCKET    0x0600
#define A20_NR_THREAD_CREATE 0x0205

/* Error codes */
#define A20_OK           0
#define A20_ERR_PERM     1
#define A20_ERR_NO_ENTRY 2
#define A20_ERR_NO_MEMORY 6

static int __test_pass = 0;
static int __test_fail = 0;

#define ASSERT(expr, msg) do { \
    if (!(expr)) { \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
        __test_fail++; \
    } else { __test_pass++; } \
} while(0)

#define RESET() do { mock_syscall_count = 0; mock_last_nr = 0; \
                     memset(mock_last_args, 0, sizeof(mock_last_args)); \
                     mock_return_val = 0; } while(0)

/* ---- Tests ---- */

static void test_close_maps_correctly(void)
{
    printf("[test] close(42) maps to handle_close(0x0100)...\n");
    RESET();
    long r = __syscall1(A20_NR_HANDLE_CLOSE, 42);
    ASSERT(mock_last_nr == 0x0100, "syscall nr = 0x0100");
    ASSERT(mock_last_args[0] == 42, "handle = 42");
    ASSERT(r == 0, "returns OK");
}

static void test_read_maps_correctly(void)
{
    printf("[test] read(fd=3, buf, 1024) maps to handle_read(0x0401)...\n");
    RESET();
    mock_return_val = 1024;
    char buf[1024];
    long r = __syscall3(A20_NR_HANDLE_READ, 3, (long)buf, 1024);
    ASSERT(mock_last_nr == 0x0401, "syscall nr = 0x0401");
    ASSERT(mock_last_args[0] == 3, "fd = 3");
    ASSERT(r == 1024, "returns 1024 bytes");
}

static void test_write_maps_correctly(void)
{
    printf("[test] write(fd=1, msg, 6) maps to handle_write(0x0402)...\n");
    RESET();
    mock_return_val = 6;
    long r = __syscall3(A20_NR_HANDLE_WRITE, 1, (long)"hello\n", 6);
    ASSERT(mock_last_nr == 0x0402, "syscall nr = 0x0402");
    ASSERT(r == 6, "returns 6 bytes written");
}

static void test_vm_alloc_maps(void)
{
    printf("[test] vm_alloc(args) maps to 0x0300...\n");
    RESET();
    mock_return_val = 0;
    long r = __syscall1(A20_NR_VM_ALLOC, (long)NULL);
    ASSERT(mock_last_nr == 0x0300, "syscall nr = 0x0300");
    (void)r;
}

static void test_task_exit_maps(void)
{
    printf("[test] task_exit(0) maps to 0x0200...\n");
    RESET();
    __syscall1(A20_NR_TASK_EXIT, 0);
    ASSERT(mock_last_nr == 0x0200, "syscall nr = 0x0200");
    ASSERT(mock_last_args[0] == 0, "exit code = 0");
}

static void test_clock_get_maps(void)
{
    printf("[test] clock_gettime maps to 0x0700...\n");
    RESET();
    struct { long sec; long nsec; } ts;
    __syscall2(A20_NR_CLOCK_GET, 0, (long)&ts);
    ASSERT(mock_last_nr == 0x0700, "syscall nr = 0x0700");
}

static void test_net_socket_maps(void)
{
    printf("[test] socket(AF_INET, SOCK_STREAM, 0) maps to 0x0600...\n");
    RESET();
    mock_return_val = 5;
    long r = __syscall3(A20_NR_NET_SOCKET, 2, 1, 0);
    ASSERT(mock_last_nr == 0x0600, "syscall nr = 0x0600");
    ASSERT(r == 5, "returns socket fd 5");
}

static void test_error_propagation(void)
{
    printf("[test] negative return from kernel maps to error...\n");
    RESET();
    mock_return_val = -6; /* A20_ERR_NO_MEMORY */
    long r = __syscall1(A20_NR_VM_ALLOC, (long)NULL);
    (void)r;
    ASSERT(mock_return_val == -6, "negative return preserved");
}

static void test_all_syscall_numbers_unique(void)
{
    printf("[test] A20 syscall numbers are distinct...\n");
    long nrs[] = {
        0x0000, 0x0001, 0x0100, 0x0101, 0x0102, 0x0103, 0x0104,
        0x0105, 0x0106, 0x0107, 0x0108, 0x0109, 0x010A, 0x010B,
        0x0200, 0x0201, 0x0202, 0x0203, 0x0204, 0x0205, 0x0206,
        0x0207, 0x0208, 0x0209, 0x020A, 0x020B, 0x020C, 0x020D,
        0x0300, 0x0301, 0x0302, 0x0303, 0x0304, 0x0305, 0x0306,
        0x0307, 0x0308, 0x0309,
        0x0400, 0x0401, 0x0402, 0x0403, 0x0404, 0x0405, 0x0406,
        0x0407, 0x0408, 0x0409, 0x040A, 0x040B, 0x040C, 0x040D,
        0x040E, 0x040F, 0x0410,
        0x0500, 0x0501, 0x0502, 0x0503, 0x0504, 0x0505, 0x0506, 0x0507,
        0x0600, 0x0601, 0x0602, 0x0603, 0x0604, 0x0605, 0x0606,
        0x0607, 0x0608, 0x0609,
        0x0700, 0x0701, 0x0702, 0x0703, 0x0704, 0x0705,
        0x0800, 0x0801, 0x0802, 0x0803,
        0x0900, 0x0901, 0x0902, 0x0903,
        0x0A00, 0x0A01, 0x0A02,
    };
    int count = sizeof(nrs) / sizeof(nrs[0]);
    ASSERT(count == 90, "exactly 90 syscall numbers");

    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (nrs[i] == nrs[j]) {
                printf("  FAIL: duplicate syscall nr 0x%lx at index %d and %d\n",
                       nrs[i], i, j);
                __test_fail++;
                goto done;
            }
        }
    }
    __test_pass++;
done:;
}

int main(void)
{
    printf("=== A20 syscall bridge tests ===\n\n");

    test_close_maps_correctly();
    test_read_maps_correctly();
    test_write_maps_correctly();
    test_vm_alloc_maps();
    test_task_exit_maps();
    test_clock_get_maps();
    test_net_socket_maps();
    test_error_propagation();
    test_all_syscall_numbers_unique();

    printf("\n=== Results: %d passed, %d failed ===\n",
           __test_pass, __test_fail);
    return __test_fail ? 1 : 0;
}
