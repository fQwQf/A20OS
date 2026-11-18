#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef struct linux_syscall_args {
    uint64_t nr;
    uint64_t arg[6];
    void *ctx;
} linux_syscall_args_t;

#include "../kernel/arch/x86_64/include/syscall_nr_x86_64.h"

static void test_unlink_flags(void)
{
    linux_syscall_args_t args = {
        .arg = { 0x1234, 0xdeadbeef, 0, 0, 0, 0 },
    };

    assert(x86_syscall_to_kernel_nr(X86_SYS_unlink) == SYS_unlinkat);
    assert(x86_syscall_rewrite_args(X86_SYS_unlink, &args) == 1);
    assert(args.arg[0] == (uint64_t)-100);
    assert(args.arg[1] == 0x1234);
    assert(args.arg[2] == 0);
}

static void test_rmdir_flags(void)
{
    linux_syscall_args_t args = {
        .arg = { 0x5678, 0xdeadbeef, 0, 0, 0, 0 },
    };

    assert(x86_syscall_to_kernel_nr(X86_SYS_rmdir) == SYS_unlinkat);
    assert(x86_syscall_rewrite_args(X86_SYS_rmdir, &args) == 1);
    assert(args.arg[0] == (uint64_t)-100);
    assert(args.arg[1] == 0x5678);
    assert(args.arg[2] == 0x200);
}

static void test_renameat_arity(void)
{
    /* r8 is caller scratch for renameat, but carries flags for renameat2. */
    assert(x86_syscall_to_kernel_nr(X86_SYS_renameat) == SYS_renameat);
    assert(x86_syscall_to_kernel_nr(X86_SYS_renameat2) == SYS_renameat2);
}

int main(void)
{
    test_unlink_flags();
    test_rmdir_flags();
    test_renameat_arity();
    puts("x86_syscall_args_test: PASS");
    return 0;
}
