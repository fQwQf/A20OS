#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

#define A(n) (args->arg[(n)])

static int64_t linux_sys_clone_call(const linux_syscall_args_t *args)
{
#if defined(CONFIG_RISCV64)
    return sys_clone(A(0), (void *)A(1), (int *)A(2), A(3), (int *)A(4));
#elif defined(CONFIG_LOONGARCH64)
    return sys_clone(A(0), (void *)A(1), (int *)A(2), A(4), (int *)A(3));
#else
    return sys_clone(A(0), (void *)A(1), (int *)A(2), A(4), (int *)A(3));
#endif
}

#define LINUX_SYSCALL(name, restores, ...) \
    static int64_t linux_handle_##name(const linux_syscall_args_t *args) \
    { \
        (void)args; \
        return __VA_ARGS__; \
    }
#include "syscall_table.def"
#undef LINUX_SYSCALL

#define LINUX_SYSCALL(name, restores, ...) \
    _Static_assert(SYS_##name < LINUX_SYSCALL_TABLE_SIZE, \
                   "Linux syscall number exceeds table size");
#include "syscall_table.def"
#undef LINUX_SYSCALL

static const linux_syscall_entry_t linux_syscall_table[LINUX_SYSCALL_TABLE_SIZE] = {
#define LINUX_SYSCALL(name, restores, ...) \
    [SYS_##name] = { SYS_##name, #name, linux_handle_##name, restores },
#include "syscall_table.def"
#undef LINUX_SYSCALL
};

#undef A

const linux_syscall_entry_t *linux_syscall_lookup(uint64_t nr)
{
    if (nr >= LINUX_SYSCALL_TABLE_SIZE)
        return NULL;
    const linux_syscall_entry_t *entry = &linux_syscall_table[nr];
    return entry->handler ? entry : NULL;
}
