#include "native_thread_shim.h"
#include "abi/native/syscall_entry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static task_t parent, child;
static struct a20_ht_internal table;
static int mode, created, destroyed, published, put_count, installed, removed;
static int waiting, wait_result, wait_status = 37 << 8;
static int expected_joinable;
int64_t sys_a20_thread_create(const a20_syscall_args_t *args);
int64_t sys_a20_task_wait(const a20_syscall_args_t *args);
task_t *proc_current(void) {
    return &parent;
}
task_t *proc_find_get(int pid) {
    (void)pid;
    return &child;
}
void proc_put(task_t *task) {
    assert(task == &child);
    put_count++;
}
void proc_free_pid(int pid) {
    assert(pid == 42);
    destroyed++;
}
void proc_publish_deferred_task(task_t *task) {
    assert(task == &child &&
           child.a20_joinable == (unsigned)expected_joinable && installed == 1);
    published++;
}
int proc_wait4_task(task_t *target, int *status, int flags) {
    assert(waiting && target == &child && flags == 0);
    *status = wait_status;
    return wait_result;
}
int proc_create_thread_deferred(uint64_t entry, uint64_t arg, vaddr_t sp,
                                vaddr_t tls, size_t size, task_t **out) {
    assert(entry == 0x1001 && arg == 77 && sp == 0x8000 && !tls &&
           size == 4096);
    if (mode == 1)
        return -ENOMEM;
    if (mode == 2)
        return -EOPNOTSUPP;
    created++;
    *out = &child;
    return 42;
}
struct a20_ht_internal *task_get_a20_ht(task_t *task) {
    assert(task == &parent);
    return mode == 6 ? NULL : &table;
}
int64_t a20_handle_install_task(struct a20_ht_internal *ht, task_t *task,
                                uint16_t type, a20_rights_t rights) {
    assert(ht == &table && task == &child && type == A20_OBJ_THREAD);
    assert(rights & A20_RIGHT_WAIT);
    if (mode == 3)
        return -A20_ERR_NO_SPACE;
    installed++;
    return 7;
}
int64_t a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t handle) {
    assert(ht == &table && handle == 7);
    removed++;
    return 0;
}
int64_t a20_handle_task_get(struct a20_ht_internal *ht, a20_handle_t handle,
                            uint16_t type, a20_rights_t rights, task_t **out) {
    if (!waiting)
        return -A20_ERR_BAD_HANDLE;
    assert(ht == &table && handle == 7 && type == A20_OBJ_INVALID &&
           rights == A20_RIGHT_WAIT);
    *out = &child;
    return 0;
}
long copy_from_user(void *dst, const void *src, size_t n) {
    if (mode == 5)
        return -EFAULT;
    memcpy(dst, src, n);
    return 0;
}
long copy_to_user(void *dst, const void *src, size_t n) {
    if (mode == 4)
        return -EFAULT;
    memcpy(dst, src, n);
    return 0;
}
int main(void) {
    static const int64_t results[] = {7,
                                      -A20_ERR_NO_MEMORY,
                                      -A20_ERR_NOT_SUPPORTED,
                                      -A20_ERR_NO_SPACE,
                                      -A20_ERR_FAULT,
                                      -A20_ERR_FAULT,
                                      -A20_ERR_BAD_HANDLE};
    for (expected_joinable = 0; expected_joinable < 2; expected_joinable++)
        for (mode = 0; mode < 7; mode++) {
            created = destroyed = published = put_count = installed = removed =
                0;
            child.a20_joinable = 0;
            a20_thread_create_args_t args = {.size = sizeof(args),
                                             .version = 1,
                                             .flags =
                                                 expected_joinable ? 1u : 0u,
                                             .entry = 0x1001,
                                             .arg = 77,
                                             .stack_base = 0x8000,
                                             .stack_size = 4096,
                                             .out_thread = 99};
            a20_syscall_args_t call = {.arg = {(uintptr_t)&args}};
            assert(sys_a20_thread_create(&call) == results[mode]);
            if (!mode) {
                assert(created == 1 && published == 1 && put_count == 1 &&
                       !destroyed && !removed && args.out_thread == 7);
            } else if (mode == 3 || mode == 4) {
                assert(created == 1 && !published && destroyed == 1 &&
                       put_count == 1 && removed == (mode == 4) &&
                       args.out_thread == 99);
            } else {
                assert(!created && !destroyed && !put_count && !published &&
                       !removed && args.out_thread == 99);
            }
        }
    waiting = 1;
    mode = 0;
    child.pid = 42;
    static const int wait_results[] = {-ERESTARTSYS, -EINTR, -ECHILD, 0, 42};
    static const int64_t wait_expected[] = {
        -A20_ERR_INTERRUPTED, -A20_ERR_INTERRUPTED, -A20_ERR_INVALID_ARGUMENT,
        -A20_ERR_WOULD_BLOCK, A20_OK};
    for (unsigned i = 0; i < sizeof(wait_results) / sizeof(wait_results[0]);
         i++) {
        a20_task_status_t status;
        memset(&status, 0xa5, sizeof(status));
        a20_task_status_t before = status;
        a20_syscall_args_t args = {.arg = {7, 0, (uintptr_t)&status}};
        wait_result = wait_results[i];
        int references = put_count;
        assert(sys_a20_task_wait(&args) == wait_expected[i]);
        assert(put_count == references + 1);
        if (wait_expected[i])
            assert(!memcmp(&before, &status, sizeof(status)));
        else
            assert(status.size == sizeof(status) && status.version == 1 &&
                   status.exit_code == 37 && status.exit_reason == 0);
    }
    for (unsigned signal = 1; signal <= 31; signal++) {
        wait_status = signal | 0x80; /* Includes the core-dump flag. */
        a20_task_status_t status;
        a20_syscall_args_t args = {.arg = {7, 0, (uintptr_t)&status}};
        assert(sys_a20_task_wait(&args) == A20_OK);
        assert(status.exit_code == (int32_t)(128 + signal) &&
               status.exit_reason == 2);
    }
    printf("PASS Native thread admission: 7 failure/publication paths; 5 wait "
           "results\n");
    return 0;
}
