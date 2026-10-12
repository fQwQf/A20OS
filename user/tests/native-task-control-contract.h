#ifndef A20_NATIVE_TASK_CONTROL_CONTRACT_H
#define A20_NATIVE_TASK_CONTROL_CONTRACT_H
#include "liba20rt/a20_task.h"
#include "liba20rt/a20_handle.h"
#include "liba20rt/a20_sync.h"
#include "liba20rt/a20_mem.h"
#include "liba20rt/a20_execution.h"
/* A real user thread: queued signals cross a checkpoint, then an external
 * force-exit races its next futex admission. Both CPUs use the public ABI. */
static struct {
    uint32_t ready, go, checkpoint, cpu, error;
    uint32_t thread_go, thread_done;
} native_control_shared;
static __attribute__((noreturn)) void native_control_fail(unsigned code) {
    a20_atomic32_store(&native_control_shared.error, code);
    a20_thread_exit(code);
    __builtin_unreachable();
}
static __attribute__((noreturn)) void native_control_worker(uintptr_t arg) {
    uint64_t old;
    if (arg != 0xa201 || a20_signal_mask((1ULL << 12) | (1ULL << 9), &old) ||
        old || a20_signal_mask((1ULL << 12) | (1ULL << 9), &old) ||
        old != (1ULL << 12) ||
        a20_atomic32_store(&native_control_shared.ready, 1))
        native_control_fail(81);
    uint32_t go = 0;
    while (!go) {
        if (a20_atomic32_load(&native_control_shared.go, &go))
            native_control_fail(82);
        if (!go) {
            a20_status_t s = a20_futex_wait(&native_control_shared.go, 0,
                                            A20_TIMEOUT_INFINITE);
            if (s && s != -A20_ERR_INTERRUPTED && s != -A20_ERR_WOULD_BLOCK)
                native_control_fail(83);
        }
    }
    if (a20_signal_check() != 0)
        native_control_fail(84);
    if (a20_signal_mask(0, &old))
        native_control_fail(86);
    if (old != (1ULL << 12))
        native_control_fail(87);
    if (a20_signal_check() != (1LL << 12))
        native_control_fail(88);
    if (a20_signal_check() != 0)
        native_control_fail(89);
    a20_thread_yield();
    if (a20_thread_get_cpu(&native_control_shared.cpu) ||
        a20_atomic32_store(&native_control_shared.checkpoint, 1))
        native_control_fail(90);
    uint32_t thread_go = 0;
    while (!thread_go) {
        if (a20_atomic32_load(&native_control_shared.thread_go, &thread_go))
            native_control_fail(91);
        if (!thread_go)
            a20_thread_yield();
    }
    const uint64_t bit = (1ULL << 12) | (1ULL << 40);
    if (a20_thread_signal_mask(bit | (1ULL << 9) | (1ULL << 19), &old, 0) ||
        old != ((1ULL << 10) | (1ULL << 41)) ||
        a20_thread_signal_check(0, 0) != 0 ||
        a20_thread_signal_check(A20_SIGNAL_SELECT | A20_SIGNAL_PEEK, bit) !=
            (int64_t)bit ||
        a20_thread_signal_check(A20_SIGNAL_SELECT | A20_SIGNAL_PEEK, bit) !=
            (int64_t)bit ||
        a20_thread_signal_check(A20_SIGNAL_SELECT, bit) != (1LL << 12) ||
        a20_thread_signal_check(A20_SIGNAL_SELECT, bit) != (1LL << 40) ||
        a20_thread_signal_check(A20_SIGNAL_SELECT, bit) != 0 ||
        a20_thread_signal_mask(0, &old, 0) || old != bit ||
        a20_atomic32_store(&native_control_shared.thread_done, 1))
        native_control_fail(92);
    for (;;) {
        a20_status_t s = a20_futex_wait(&native_control_shared.ready, 1,
                                        A20_TIMEOUT_INFINITE);
        if (s && s != -A20_ERR_INTERRUPTED && s != -A20_ERR_WOULD_BLOCK)
            native_control_fail(85);
    }
}
static int native_control_wait(uint32_t *word) {
    for (unsigned i = 0; i < 100000; i++) {
        uint32_t value;
        if (a20_atomic32_load(word, &value))
            return 0;
        if (value)
            return 1;
        a20_thread_yield();
    }
    return 0;
}
static int native_task_control_contract(void) {
#define C_CHECK(test, code)                                                    \
    do {                                                                       \
        if (!(test))                                                           \
            return code;                                                       \
    } while (0)
    /* Earlier child exits legitimately queue SIGCHLD in this shared table. */
    C_CHECK(a20_signal_check() >= 0, 20);
    uint64_t saved_thread_mask;
    C_CHECK(a20_thread_signal_mask((1ULL << 10) | (1ULL << 41),
                                   &saved_thread_mask, 0) == A20_OK,
            21);
    uint64_t stack;
    C_CHECK(a20_vm_alloc_pages(4, 3, &stack) == A20_OK, 1);
    a20_thread_create_args_t args = {.entry = (uintptr_t)native_control_worker,
                                     .arg = 0xa201,
                                     .stack_base = stack + 16384,
                                     .stack_size = 16384};
    a20_signal_control_t control = {sizeof(control),
                                    1,
                                    A20_SIGNAL_CONTROL_CHECK,
                                    A20_SIGNAL_THREAD | (1U << 24),
                                    0,
                                    0,
                                    0};
    C_CHECK(sizeof(control) == 40 && a20_thread_signal_control(&control) ==
                                         -A20_ERR_INVALID_ARGUMENT,
            28);
    control.flags = A20_SIGNAL_THREAD;
    control.size = 39;
    C_CHECK(a20_thread_signal_control(&control) == -A20_ERR_INVALID_ARGUMENT,
            29);
    control.size = sizeof(control);
    control.version = 0;
    C_CHECK(a20_thread_signal_control(&control) == -A20_ERR_INVALID_ARGUMENT,
            30);
    C_CHECK(a20_thread_create(&args) >= 0, 2);
    /* Unknown register-width flags must fail on every architecture. */
    C_CHECK(a20_syscall6(A20_SYS_task_kill, args.out_thread, 0, 1ULL << 24, 0,
                         0, 0) == -A20_ERR_INVALID_ARGUMENT &&
                a20_syscall6(A20_SYS_signal_check,
                             A20_SIGNAL_THREAD | (1ULL << 24), 0, 0, 0, 0,
                             0) == -A20_ERR_INVALID_ARGUMENT &&
                a20_syscall6(A20_SYS_signal_mask, 0, 0,
                             A20_SIGNAL_THREAD | (1ULL << 24), 0, 0,
                             0) == -A20_ERR_INVALID_ARGUMENT &&
                a20_syscall6(A20_SYS_signal_check, A20_SIGNAL_PEEK, 0, 0, 0, 0,
                             0) == -A20_ERR_INVALID_ARGUMENT,
            27);
    C_CHECK(native_control_wait(&native_control_shared.ready), 3);
    a20_handle_identity_t before, after;
    C_CHECK(a20_hdl_identity(args.out_thread, &before) == A20_OK &&
                before.state == A20_OBJECT_LIVE,
            4);
    C_CHECK(a20_task_kill(args.out_thread, 64) == -A20_ERR_INVALID_ARGUMENT &&
                a20_task_kill(args.out_thread, 0) == A20_OK,
            5);
    C_CHECK(a20_task_wait(args.out_thread, A20_TASK_WAIT_NONBLOCK, 0) ==
                -A20_ERR_WOULD_BLOCK,
            6);
    a20_execution_domain_info_t domain;
    uint32_t cpu;
    C_CHECK(a20_execution_domain_info(&domain) == A20_OK &&
                a20_thread_get_cpu(&cpu) == A20_OK,
            7);
    uint32_t target = cpu;
    for (unsigned i = 0; i < domain.cpu_capacity; i++)
        if (i != cpu && (domain.online_mask & (1U << i))) {
            target = i;
            break;
        }
    a20_sched_args_t sched = {.flags = A20_SCHED_AFFINITY,
                              .affinity = 1U << target,
                              .affinity_size = sizeof(uint64_t)};
    C_CHECK(a20_sched_set(args.out_thread, &sched) == A20_OK, 8);
    C_CHECK(a20_task_kill(args.out_thread, 12) == A20_OK &&
                a20_atomic32_store(&native_control_shared.go, 1) == A20_OK,
            9);
    uint32_t woken;
    C_CHECK(a20_futex_wake(&native_control_shared.go, 1, &woken) == A20_OK, 10);
    if (!native_control_wait(&native_control_shared.checkpoint)) {
        uint32_t error;
        C_CHECK(a20_atomic32_load(&native_control_shared.error, &error) ==
                    A20_OK,
                18);
        return error ? (int)error : 19;
    }
    C_CHECK(native_control_shared.cpu == target, 17);
    C_CHECK(a20_thread_signal(args.out_thread, 12) == A20_OK &&
                a20_thread_signal(args.out_thread, 40) == A20_OK,
            22);
    C_CHECK(a20_thread_signal_check(A20_SIGNAL_SELECT | A20_SIGNAL_PEEK,
                                    1ULL << 12) == 0 &&
                a20_signal_check() == 0,
            23);
    uint64_t current_mask;
    C_CHECK(a20_thread_signal_mask(0, &current_mask, A20_SIGNAL_MASK_QUERY) ==
                    A20_OK &&
                current_mask == ((1ULL << 10) | (1ULL << 41)),
            24);
    C_CHECK(a20_atomic32_store(&native_control_shared.thread_go, 1) == A20_OK &&
                native_control_wait(&native_control_shared.thread_done),
            25);
    C_CHECK(a20_thread_signal_mask(saved_thread_mask, 0, 0) == A20_OK, 26);
    C_CHECK(a20_task_kill(args.out_thread, 9) == A20_OK, 11);
    a20_task_status_t status;
    C_CHECK(a20_task_wait(args.out_thread, 0, &status) == A20_OK &&
                status.exit_code == 137,
            12);
    C_CHECK(a20_hdl_identity(args.out_thread, &after) == A20_OK &&
                after.state == A20_OBJECT_RETIRED &&
                a20_object_id_equal(&before.id, &after.id),
            13);
    C_CHECK(a20_task_kill(args.out_thread, 9) == -A20_ERR_BAD_HANDLE, 14);
    C_CHECK(a20_hdl_close(args.out_thread) == A20_OK &&
                a20_vm_unmap(stack, 16384) == A20_OK,
            15);
    C_CHECK(a20_task_kill(args.out_thread, 0) == -A20_ERR_BAD_HANDLE, 16);
    return 0;
#undef C_CHECK
}
#endif
