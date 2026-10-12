#ifndef A20_NATIVE_THREAD_CONTRACT_H
#define A20_NATIVE_THREAD_CONTRACT_H
#include "liba20rt/a20_task.h"
#include "liba20rt/a20_handle.h"
#include "liba20rt/a20_sync.h"
#include "liba20rt/a20_mem.h"
#include "liba20rt/a20_execution.h"
#include "native-runtime-contract.h"
#include "native-object-contract.h"
/* Identical Native application code on MCU and server ISAs. No host task
 * manipulation, no reliance on user CPU atomics or libc allocator locking. */
static struct {
    uint32_t ready, go, nested, expected_cpu, worker_cpu, parent_cpu;
    uint64_t nested_serial;
} native_thread_shared;
static __attribute__((noreturn)) void native_thread_nested(uintptr_t arg) {
    if (arg != 0xa20 || a20_atomic32_store(&native_thread_shared.nested, 19))
        a20_thread_exit(91);
    a20_thread_exit(19);
    __builtin_unreachable();
}
static __attribute__((noreturn)) void native_thread_worker(uintptr_t arg) {
    volatile uintptr_t local = arg;
    if (!native_runtime_matches())
        a20_thread_exit(89);
    if (a20_atomic32_store(&native_thread_shared.ready, 1))
        a20_thread_exit(92);
    uint32_t value;
    for (;;) {
        if (a20_atomic32_load(&native_thread_shared.go, &value))
            a20_thread_exit(93);
        if (value)
            break;
        a20_status_t st =
            a20_futex_wait(&native_thread_shared.go, 0, A20_TIMEOUT_INFINITE);
        if (st != A20_OK && st != -A20_ERR_WOULD_BLOCK)
            a20_thread_exit(94);
    }
    a20_thread_yield();
    uint32_t cpu;
    if (a20_thread_get_cpu(&cpu) || cpu != native_thread_shared.expected_cpu)
        a20_thread_exit(90);
    native_thread_shared.worker_cpu = cpu;
    if (local != 0xc0ffee)
        a20_thread_exit(95);
    uint64_t stack;
    if (a20_vm_alloc_pages(4, 3, &stack))
        a20_thread_exit(96);
    a20_thread_create_args_t args = {.entry = (uintptr_t)native_thread_nested,
                                     .arg = 0xa20,
                                     .stack_base = stack + 16384,
                                     .stack_size = 16384};
    a20_status_t handle = a20_thread_create(&args);
    if (handle < 0 || (uint32_t)handle != args.out_thread)
        a20_thread_exit(97);
    a20_handle_identity_t nested_identity;
    if (!native_runtime_identity(args.out_thread, &nested_identity,
                                 A20_OBJECT_LIVE))
        a20_thread_exit(88);
    native_thread_shared.nested_serial = nested_identity.id.serial;
    a20_task_status_t status;
    if (a20_task_wait(args.out_thread, 0, &status) || status.exit_code != 19 ||
        a20_hdl_close(args.out_thread) || a20_vm_unmap(stack, 16384))
        a20_thread_exit(98);
    a20_thread_exit(37);
    __builtin_unreachable();
}
static int native_thread_contract(void) {
#define T_CHECK(test, code)                                                    \
    do {                                                                       \
        if (!(test))                                                           \
            return code;                                                       \
    } while (0)
    _Static_assert(sizeof(a20_thread_create_args_t) == 56, "thread ABI");
    a20_execution_info_t execution;
    T_CHECK(a20_execution_info(&execution) == A20_OK &&
                (execution.features & A20_EXEC_THREAD_CREATE),
            1);
    uint64_t stack = 0;
    T_CHECK(a20_vm_alloc_pages(4, 3, &stack) == A20_OK, 2);
    a20_thread_create_args_t args = {.entry = (uintptr_t)native_thread_worker,
                                     .arg = 0xc0ffee,
                                     .stack_base = stack + 16384,
                                     .stack_size = 16384};
    a20_thread_create_args_t bad = args;
    bad.flags = 1;
    T_CHECK(a20_thread_create(&bad) == -A20_ERR_INVALID_ARGUMENT, 3);
    bad = args;
    bad.entry = 0;
    T_CHECK(a20_thread_create(&bad) == -A20_ERR_INVALID_ARGUMENT, 4);
    bad = args;
    bad.stack_base--;
    T_CHECK(a20_thread_create(&bad) == -A20_ERR_INVALID_ARGUMENT, 5);
    if (execution.pointer_bits == 32) {
        bad = args;
        bad.arg |= 1ULL << 32;
        T_CHECK(a20_thread_create(&bad) == -A20_ERR_INVALID_ARGUMENT, 6);
    }
    if (execution.memory_model == A20_EXEC_MEMORY_FLAT) {
        bad = args;
        bad.tls_base = 16;
        T_CHECK(a20_thread_create(&bad) == -A20_ERR_NOT_SUPPORTED, 7);
        bad = args;
        bad.entry = stack | 1;
        T_CHECK(a20_thread_create(&bad) == -A20_ERR_FAULT, 8);
        bad = args;
        bad.stack_size = 16;
        T_CHECK(a20_thread_create(&bad) == -A20_ERR_INVALID_ARGUMENT, 9);
    }
    a20_status_t handle = a20_thread_create(&args);
    T_CHECK(handle >= 0 && (uint32_t)handle == args.out_thread, 10);
    a20_handle_identity_t original_identity;
    T_CHECK(native_runtime_identity(args.out_thread, &original_identity,
                                    A20_OBJECT_LIVE),
            29);
    T_CHECK(native_object_begin(args.out_thread) == 0, 34);
    uint32_t ready = 0;
    for (unsigned i = 0; i < 100000 && !ready; i++) {
        T_CHECK(a20_atomic32_load(&native_thread_shared.ready, &ready) ==
                    A20_OK,
                11);
        if (!ready)
            a20_thread_yield();
    }
    T_CHECK(ready == 1, 12);
    a20_handle_t duplicate = A20_HANDLE_NULL;
    if (execution.features & A20_EXEC_HANDLES) {
        a20_handle_dup_args_t dup = {.source = args.out_thread,
                                     .rights_mask =
                                         A20_RIGHT_WAIT | A20_RIGHT_STAT};
        T_CHECK(a20_hdl_dup(&dup) == A20_OK, 26);
        duplicate = dup.out_handle;
        a20_handle_identity_t alias;
        T_CHECK(native_runtime_identity(duplicate, &alias, A20_OBJECT_LIVE) &&
                    a20_object_id_equal(&original_identity.id, &alias.id),
                30);
    }
    T_CHECK(a20_task_wait(args.out_thread, A20_TASK_WAIT_NONBLOCK, 0) ==
                -A20_ERR_WOULD_BLOCK,
            13);
    if (execution.memory_model == A20_EXEC_MEMORY_FLAT)
        T_CHECK(a20_vm_unmap(stack, 16384) == -A20_ERR_BUSY, 14);
    a20_execution_domain_info_t domain;
    uint32_t cpu;
    T_CHECK(a20_execution_domain_info(&domain) == A20_OK &&
                a20_thread_get_cpu(&cpu) == A20_OK,
            23);
    native_thread_shared.parent_cpu = cpu;
    uint32_t target = cpu;
    for (unsigned i = 0; i < domain.cpu_capacity; i++)
        if (i != cpu && (domain.online_mask & (1U << i))) {
            target = i;
            break;
        }
    native_thread_shared.expected_cpu = target;
    a20_sched_args_t sched = {.flags = A20_SCHED_AFFINITY,
                              .affinity = 1U << target,
                              .affinity_size = sizeof(uint64_t)};
    T_CHECK(a20_sched_set(args.out_thread, &sched) == A20_OK, 24);
    T_CHECK(a20_sched_get(args.out_thread, &sched) == A20_OK &&
                sched.affinity == (1U << target),
            25);
    T_CHECK(a20_atomic32_store(&native_thread_shared.go, 1) == A20_OK, 15);
    uint32_t woken;
    T_CHECK(a20_futex_wake(&native_thread_shared.go, 1, &woken) == A20_OK, 16);
    a20_task_status_t status;
    T_CHECK(a20_task_wait(args.out_thread, 0, &status) == A20_OK &&
                status.size == sizeof(status) && status.version == 1 &&
                status.exit_code == 37,
            17);
    T_CHECK(a20_atomic32_load(&native_thread_shared.nested, &ready) == A20_OK &&
                ready == 19,
            18);
    a20_handle_identity_t retired;
    T_CHECK(native_runtime_identity(args.out_thread, &retired,
                                    A20_OBJECT_RETIRED) &&
                a20_object_id_equal(&original_identity.id, &retired.id) &&
                native_thread_shared.nested_serial >
                    original_identity.id.serial,
            31);
    uint8_t wire[64];
    a20_object_id_t decoded;
    T_CHECK(!a20_object_id_encode(wire, sizeof(wire), &retired.id) &&
                !a20_object_id_decode(wire, sizeof(wire), &decoded) &&
                a20_object_id_equal(&decoded, &original_identity.id),
            32);
    /* Reaped identity remains safely closeable but cannot resolve a task. */
    T_CHECK(a20_task_wait(args.out_thread, A20_TASK_WAIT_NONBLOCK, 0) ==
                    -A20_ERR_BAD_HANDLE &&
                a20_sched_get(args.out_thread, &sched) == -A20_ERR_BAD_HANDLE,
            27);
    if (duplicate != A20_HANDLE_NULL) {
        a20_handle_identity_t alias;
        T_CHECK(
            native_runtime_identity(duplicate, &alias, A20_OBJECT_RETIRED) &&
                a20_object_id_equal(&original_identity.id, &alias.id),
            33);
        T_CHECK(a20_task_wait(duplicate, A20_TASK_WAIT_NONBLOCK, 0) ==
                        -A20_ERR_BAD_HANDLE &&
                    a20_hdl_close(duplicate) == A20_OK,
                28);
    }
    T_CHECK(native_object_finish(args.out_thread) == 0, 35);
    T_CHECK(a20_hdl_close(args.out_thread) == -A20_ERR_BAD_HANDLE, 20);
    T_CHECK(a20_task_wait(args.out_thread, 0, 0) == -A20_ERR_BAD_HANDLE, 21);
    T_CHECK(a20_vm_unmap(stack, 16384) == A20_OK, 22);
#undef T_CHECK
    return 0;
}
#endif
