/*
 * A20OS Native ABI syscall dispatch table.
 * Uses the same X-macro pattern as abi/linux/syscall_table.c.
 */
#include "abi/native/syscall_entry.h"
#include "abi/native/errno.h"
#include "core/klog.h"

#define A20_ARG(n) (args->arg[(n)])

/* Forward declarations for implemented syscalls */
int64_t sys_a20_abi_info(const a20_syscall_args_t *args);
int64_t sys_a20_feature_test(const a20_syscall_args_t *args);
int64_t sys_a20_handle_close(const a20_syscall_args_t *args);
int64_t sys_a20_handle_dup(const a20_syscall_args_t *args);
int64_t sys_a20_handle_query(const a20_syscall_args_t *args);
int64_t sys_a20_handle_replace(const a20_syscall_args_t *args);
int64_t sys_a20_handle_close_many(const a20_syscall_args_t *args);
int64_t sys_a20_handle_seek(const a20_syscall_args_t *args);
int64_t sys_a20_task_exit(const a20_syscall_args_t *args);
int64_t sys_a20_task_spawn(const a20_syscall_args_t *args);
int64_t sys_a20_task_wait(const a20_syscall_args_t *args);
int64_t sys_a20_vm_alloc(const a20_syscall_args_t *args);
int64_t sys_a20_vm_unmap(const a20_syscall_args_t *args);
int64_t sys_a20_path_open(const a20_syscall_args_t *args);
int64_t sys_a20_handle_read(const a20_syscall_args_t *args);
int64_t sys_a20_handle_write(const a20_syscall_args_t *args);
int64_t sys_a20_handle_stat(const a20_syscall_args_t *args);
int64_t sys_a20_clock_get(const a20_syscall_args_t *args);

/* Forward declarations for Phase 2 syscalls (sys_phase2.c) */
int64_t sys_a20_handle_transfer(const a20_syscall_args_t *args);
int64_t sys_a20_handle_set_meta(const a20_syscall_args_t *args);
int64_t sys_a20_handle_xattr_set(const a20_syscall_args_t *args);
int64_t sys_a20_handle_xattr_get(const a20_syscall_args_t *args);
int64_t sys_a20_handle_xattr_list(const a20_syscall_args_t *args);
int64_t sys_a20_handle_xattr_remove(const a20_syscall_args_t *args);
int64_t sys_a20_task_kill(const a20_syscall_args_t *args);
int64_t sys_a20_task_info(const a20_syscall_args_t *args);
int64_t sys_a20_thread_create(const a20_syscall_args_t *args);
int64_t sys_a20_thread_exit(const a20_syscall_args_t *args);
int64_t sys_a20_thread_sleep(const a20_syscall_args_t *args);
int64_t sys_a20_thread_yield(const a20_syscall_args_t *args);
int64_t sys_a20_task_set_sched(const a20_syscall_args_t *args);
int64_t sys_a20_task_get_sched(const a20_syscall_args_t *args);
int64_t sys_a20_task_get_limits(const a20_syscall_args_t *args);
int64_t sys_a20_task_set_limits(const a20_syscall_args_t *args);
int64_t sys_a20_task_get_usage(const a20_syscall_args_t *args);
int64_t sys_a20_vm_protect(const a20_syscall_args_t *args);
int64_t sys_a20_vm_map(const a20_syscall_args_t *args);
int64_t sys_a20_vm_share(const a20_syscall_args_t *args);
int64_t sys_a20_vm_flush(const a20_syscall_args_t *args);
int64_t sys_a20_vm_advise(const a20_syscall_args_t *args);
int64_t sys_a20_vm_remap(const a20_syscall_args_t *args);
int64_t sys_a20_vm_lock(const a20_syscall_args_t *args);
int64_t sys_a20_vm_create_object(const a20_syscall_args_t *args);
int64_t sys_a20_path_create(const a20_syscall_args_t *args);
int64_t sys_a20_path_unlink(const a20_syscall_args_t *args);
int64_t sys_a20_path_rename(const a20_syscall_args_t *args);
int64_t sys_a20_handle_control(const a20_syscall_args_t *args);
int64_t sys_a20_path_readdir(const a20_syscall_args_t *args);
int64_t sys_a20_path_link(const a20_syscall_args_t *args);
int64_t sys_a20_path_symlink(const a20_syscall_args_t *args);
int64_t sys_a20_path_readlink(const a20_syscall_args_t *args);
int64_t sys_a20_path_resolve(const a20_syscall_args_t *args);
int64_t sys_a20_fs_stat(const a20_syscall_args_t *args);
int64_t sys_a20_fs_mount(const a20_syscall_args_t *args);
int64_t sys_a20_fs_umount(const a20_syscall_args_t *args);
int64_t sys_a20_fs_sync(const a20_syscall_args_t *args);
int64_t sys_a20_event_queue_create(const a20_syscall_args_t *args);
int64_t sys_a20_event_watch(const a20_syscall_args_t *args);
int64_t sys_a20_event_wait(const a20_syscall_args_t *args);
int64_t sys_a20_event_cancel(const a20_syscall_args_t *args);
int64_t sys_a20_channel_create(const a20_syscall_args_t *args);
int64_t sys_a20_channel_send(const a20_syscall_args_t *args);
int64_t sys_a20_channel_recv(const a20_syscall_args_t *args);
int64_t sys_a20_event_watch_fs(const a20_syscall_args_t *args);
int64_t sys_a20_net_socket(const a20_syscall_args_t *args);
int64_t sys_a20_net_bind(const a20_syscall_args_t *args);
int64_t sys_a20_net_connect(const a20_syscall_args_t *args);
int64_t sys_a20_net_accept(const a20_syscall_args_t *args);
int64_t sys_a20_net_listen(const a20_syscall_args_t *args);
int64_t sys_a20_net_sendmsg(const a20_syscall_args_t *args);
int64_t sys_a20_net_recvmsg(const a20_syscall_args_t *args);
int64_t sys_a20_net_socketpair(const a20_syscall_args_t *args);
int64_t sys_a20_net_getname(const a20_syscall_args_t *args);
int64_t sys_a20_net_shutdown(const a20_syscall_args_t *args);
int64_t sys_a20_timer_create(const a20_syscall_args_t *args);
int64_t sys_a20_timer_set(const a20_syscall_args_t *args);
int64_t sys_a20_timer_cancel(const a20_syscall_args_t *args);
int64_t sys_a20_clock_set(const a20_syscall_args_t *args);
int64_t sys_a20_clock_resolution(const a20_syscall_args_t *args);
int64_t sys_a20_ns_create(const a20_syscall_args_t *args);
int64_t sys_a20_ns_apply(const a20_syscall_args_t *args);
int64_t sys_a20_security_get_context(const a20_syscall_args_t *args);
int64_t sys_a20_security_set_context(const a20_syscall_args_t *args);
int64_t sys_a20_debug_attach(const a20_syscall_args_t *args);
int64_t sys_a20_debug_read_regs(const a20_syscall_args_t *args);
int64_t sys_a20_debug_write_regs(const a20_syscall_args_t *args);
int64_t sys_a20_debug_map_memory(const a20_syscall_args_t *args);
int64_t sys_a20_system_info(const a20_syscall_args_t *args);
int64_t sys_a20_system_random(const a20_syscall_args_t *args);
int64_t sys_a20_system_reboot(const a20_syscall_args_t *args);

/* Generate handler stubs from .def */
#define A20_NATIVE_SYSCALL(name, ...) \
    static int64_t a20_handle_##name(const a20_syscall_args_t *args) \
    { (void)args; return __VA_ARGS__; }
#include "syscall_table.def"
#undef A20_NATIVE_SYSCALL

/* Build the dispatch table: indexed by syscall number.
 * Native ABI uses sparse 16-bit numbers (0x0000..0x0A02),
 * so we use a hash-style lookup rather than a flat array. */

struct a20_table_entry {
    uint64_t nr;
    const char *name;
    a20_syscall_handler_t handler;
};

static const struct a20_table_entry a20_syscall_table[] = {
#define A20_NATIVE_SYSCALL(name, ...) \
    { A20_SYS_##name, #name, a20_handle_##name },
#include "syscall_table.def"
#undef A20_NATIVE_SYSCALL
};

#define A20_TABLE_SIZE (sizeof(a20_syscall_table) / sizeof(a20_syscall_table[0]))

const a20_syscall_entry_t *a20_syscall_lookup(uint64_t nr)
{
    for (uint64_t i = 0; i < A20_TABLE_SIZE; i++) {
        if (a20_syscall_table[i].nr == nr) {
            static a20_syscall_entry_t result;
            result.nr = a20_syscall_table[i].nr;
            result.name = a20_syscall_table[i].name;
            result.handler = a20_syscall_table[i].handler;
            return &result;
        }
    }
    return NULL;
}
