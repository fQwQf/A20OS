#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

#define A(n) (args->arg[(n)])

static int64_t linux_sys_clone_call(const linux_syscall_args_t *args)
{
    linux_syscall_args_t a = *args;
    arch_adjust_clone_args(&a);
    return sys_clone(a.arg[0], (void *)(uintptr_t)a.arg[1],
                     (int *)(uintptr_t)a.arg[2], a.arg[3],
                     (int *)(uintptr_t)a.arg[4]);
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

/* Some syscall numbers alias by design (e.g. SYS_inotify_init ==
 * SYS_inotify_init1 on several 64-bit ABIs): the later initializer
 * intentionally wins the designated table slot. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
static const linux_syscall_entry_t linux_syscall_table[LINUX_SYSCALL_TABLE_SIZE] = {
#define LINUX_SYSCALL(name, restores, ...) \
    [SYS_##name] = { SYS_##name, #name, linux_handle_##name, restores },
#include "syscall_table.def"
#undef LINUX_SYSCALL
};
#pragma GCC diagnostic pop

#undef A

const linux_syscall_entry_t *linux_syscall_lookup(uint64_t nr)
{
    if (nr >= LINUX_SYSCALL_TABLE_SIZE)
        return NULL;
    const linux_syscall_entry_t *entry = &linux_syscall_table[nr];
    return entry->handler ? entry : NULL;
}
