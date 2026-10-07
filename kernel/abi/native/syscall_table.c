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
int64_t sys_a20_task_clone(const a20_syscall_args_t *args);
int64_t sys_a20_execve(const a20_syscall_args_t *args);
int64_t sys_a20_task_adopt(const a20_syscall_args_t *args);
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
int64_t sys_a20_handle_poll(const a20_syscall_args_t *args);
int64_t sys_a20_task_kill(const a20_syscall_args_t *args);
int64_t sys_a20_signal_check(const a20_syscall_args_t *args);
int64_t sys_a20_signal_mask(const a20_syscall_args_t *args);
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
int64_t sys_a20_thread_get_cpu(const a20_syscall_args_t *args);
int64_t sys_a20_vm_protect(const a20_syscall_args_t *args);
int64_t sys_a20_vm_map(const a20_syscall_args_t *args);
int64_t sys_a20_vm_share(const a20_syscall_args_t *args);
int64_t sys_a20_vm_flush(const a20_syscall_args_t *args);
int64_t sys_a20_vm_advise(const a20_syscall_args_t *args);
int64_t sys_a20_vm_remap(const a20_syscall_args_t *args);
int64_t sys_a20_vm_create_vmar(const a20_syscall_args_t *args);
int64_t sys_a20_vm_lock(const a20_syscall_args_t *args);
int64_t sys_a20_vm_create_object(const a20_syscall_args_t *args);
int64_t sys_a20_vm_share_region(const a20_syscall_args_t *args);
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
int64_t sys_a20_fs_serve(const a20_syscall_args_t *args);
int64_t sys_a20_fs_block_io(const a20_syscall_args_t *args);
int64_t sys_a20_path_unlink_at(const a20_syscall_args_t *args);
int64_t sys_a20_path_rename_at(const a20_syscall_args_t *args);
int64_t sys_a20_path_link_at(const a20_syscall_args_t *args);
int64_t sys_a20_path_symlink_at(const a20_syscall_args_t *args);
int64_t sys_a20_path_readlink_at(const a20_syscall_args_t *args);
int64_t sys_a20_event_queue_create(const a20_syscall_args_t *args);
int64_t sys_a20_event_watch(const a20_syscall_args_t *args);
int64_t sys_a20_event_wait(const a20_syscall_args_t *args);
int64_t sys_a20_event_cancel(const a20_syscall_args_t *args);
int64_t sys_a20_channel_create(const a20_syscall_args_t *args);
int64_t sys_a20_channel_send(const a20_syscall_args_t *args);
int64_t sys_a20_channel_recv(const a20_syscall_args_t *args);
int64_t sys_a20_event_watch_fs(const a20_syscall_args_t *args);
int64_t sys_a20_channel_call(const a20_syscall_args_t *args);
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
/* Debug (0x0900) — sys_native_debug.c, wraps proc_debug_* */
int64_t sys_a20_debug_attach(const a20_syscall_args_t *args);
int64_t sys_a20_debug_read_regs(const a20_syscall_args_t *args);
int64_t sys_a20_debug_write_regs(const a20_syscall_args_t *args);
int64_t sys_a20_debug_map_memory(const a20_syscall_args_t *args);
int64_t sys_a20_debug_traceme(const a20_syscall_args_t *args);
int64_t sys_a20_debug_wait(const a20_syscall_args_t *args);
int64_t sys_a20_debug_resume(const a20_syscall_args_t *args);
int64_t sys_a20_debug_detach(const a20_syscall_args_t *args);
int64_t sys_a20_debug_event(const a20_syscall_args_t *args);
int64_t sys_a20_debug_read(const a20_syscall_args_t *args);
int64_t sys_a20_debug_write(const a20_syscall_args_t *args);
int64_t sys_a20_debug_kill(const a20_syscall_args_t *args);

/* Kernel extension points (0x0D00) — sys_native_ext.c */
int64_t sys_a20_ext_prog_load(const a20_syscall_args_t *args);
int64_t sys_a20_ext_prog_attach(const a20_syscall_args_t *args);
int64_t sys_a20_ext_prog_detach(const a20_syscall_args_t *args);
int64_t sys_a20_ext_prog_release(const a20_syscall_args_t *args);
int64_t sys_a20_ext_point_info(const a20_syscall_args_t *args);
int64_t sys_a20_system_info(const a20_syscall_args_t *args);
int64_t sys_a20_system_random(const a20_syscall_args_t *args);
int64_t sys_a20_system_reboot(const a20_syscall_args_t *args);
int64_t sys_a20_registry_claim(const a20_syscall_args_t *args);
int64_t sys_a20_futex_wait(const a20_syscall_args_t *args);
int64_t sys_a20_futex_wake(const a20_syscall_args_t *args);
int64_t sys_a20_device_map_mmio(const a20_syscall_args_t *args);
int64_t sys_a20_device_irq_listen(const a20_syscall_args_t *args);
int64_t sys_a20_device_irq_ack(const a20_syscall_args_t *args);
int64_t sys_a20_device_irq_unlisten(const a20_syscall_args_t *args);
int64_t sys_a20_device_vmo_phys(const a20_syscall_args_t *args);
int64_t sys_a20_device_block_attach(const a20_syscall_args_t *args);
int64_t sys_a20_device_block_complete(const a20_syscall_args_t *args);
int64_t sys_a20_device_claim(const a20_syscall_args_t *args);
int64_t sys_a20_device_release(const a20_syscall_args_t *args);
int64_t sys_a20_device_alloc_dma(const a20_syscall_args_t *args);
int64_t sys_a20_device_free_dma(const a20_syscall_args_t *args);
int64_t sys_a20_device_get_info(const a20_syscall_args_t *args);
int64_t sys_a20_pager_create(const a20_syscall_args_t *args);
int64_t sys_a20_pager_vmo_attach(const a20_syscall_args_t *args);
int64_t sys_a20_pager_supply_pages(const a20_syscall_args_t *args);
int64_t sys_a20_monitor_create(const a20_syscall_args_t *args);
int64_t sys_a20_monitor_query(const a20_syscall_args_t *args);
int64_t sys_a20_task_mem_read(const a20_syscall_args_t *args);
int64_t sys_a20_task_mem_write(const a20_syscall_args_t *args);

/* Forward declarations for Cluster syscalls (sys_native_cluster.c) */
int64_t sys_a20_cluster_set_self(const a20_syscall_args_t *args);
int64_t sys_a20_cluster_export(const a20_syscall_args_t *args);
int64_t sys_a20_cluster_connect(const a20_syscall_args_t *args);
int64_t sys_a20_cluster_route(const a20_syscall_args_t *args);
int64_t sys_a20_cluster_event_subscribe(const a20_syscall_args_t *args);
int64_t sys_a20_cluster_link_status(const a20_syscall_args_t *args);

/* Generate handler stubs from .def */
#define A20_NATIVE_SYSCALL(name, ...) \
    static int64_t a20_handle_##name(const a20_syscall_args_t *args) \
    { (void)args; return __VA_ARGS__; }
#include "syscall_table.def"
#undef A20_NATIVE_SYSCALL

/* Build the dispatch table: direct-indexed by syscall number.
 * Native ABI numbers are sparse 16-bit values (class<<8 | index), so a flat
 * pointer index of A20_SYSCALL_TABLE_SIZE turns dispatch into one load and
 * a bounds check. */

/* One const entry per syscall, so the index below can point into it. */
#define A20_NATIVE_SYSCALL(name, ...) \
    static const a20_syscall_entry_t a20_entry_##name = \
        { A20_SYS_##name, #name, a20_handle_##name };
#include "syscall_table.def"
#undef A20_NATIVE_SYSCALL

#define A20_NATIVE_SYSCALL(name, ...) \
    _Static_assert(A20_SYS_##name < A20_SYSCALL_TABLE_SIZE, \
                   "Native syscall number exceeds the dispatch index");
#include "syscall_table.def"
#undef A20_NATIVE_SYSCALL

/* Every lookup returns a pointer into this array, so it must stay const: a
 * shared mutable copy would be one data race between two CPUs in syscalls. */
static const a20_syscall_entry_t *const a20_syscall_index[A20_SYSCALL_TABLE_SIZE] = {
#define A20_NATIVE_SYSCALL(name, ...) [A20_SYS_##name] = &a20_entry_##name,
#include "syscall_table.def"
#undef A20_NATIVE_SYSCALL
};

const a20_syscall_entry_t *a20_syscall_lookup(uint64_t nr)
{
    if (nr >= A20_SYSCALL_TABLE_SIZE)
        return NULL;
    return a20_syscall_index[nr];
}
