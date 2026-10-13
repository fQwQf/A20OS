#ifndef A20_NATIVE_COMPUTE_CANCEL_CONTRACT_H
#define A20_NATIVE_COMPUTE_CANCEL_CONTRACT_H
#include "liba20rt/a20_task.h"
#include "liba20rt/a20_handle.h"
#include "liba20rt/a20_sync.h"
#include "liba20rt/a20_mem.h"
#include "liba20rt/a20_execution.h"
/* Distinct real instruction ranges, with no calls or syscalls in either loop.
 * The MCU checker counts their actual user instructions on each CPU. */
static __attribute__((noinline, noreturn)) void native_compute_loop0(void) {
    volatile uint32_t x = 0x12345678;
    for (;;)
        x = (x * 1664525U + 1013904223U) ^ (x >> 7);
}
static __attribute__((noinline, noreturn)) void native_compute_loop1(void) {
    volatile uint32_t x = 0x87654321;
    for (;;)
        x = (x * 22695477U + 1U) ^ (x >> 9);
}
static struct {
    uint32_t ready[2], go;
} native_compute_shared;
static __attribute__((noreturn)) void native_compute_worker(uintptr_t index) {
    if (index > 1 || a20_atomic32_store(&native_compute_shared.ready[index], 1))
        a20_thread_exit(81);
    uint32_t woken;
    if (a20_futex_wake(&native_compute_shared.ready[index], 1, &woken))
        a20_thread_exit(84);
    uint32_t go = 0;
    while (!go) {
        if (a20_atomic32_load(&native_compute_shared.go, &go))
            a20_thread_exit(82);
        if (!go) {
            a20_status_t s = a20_futex_wait(&native_compute_shared.go, 0,
                                            A20_TIMEOUT_INFINITE);
            if (s && s != -A20_ERR_INTERRUPTED && s != -A20_ERR_WOULD_BLOCK)
                a20_thread_exit(83);
        }
    }
    if (index)
        native_compute_loop1();
    native_compute_loop0();
}
static int native_compute_cancel_contract(void) {
#define P_CHECK(test, code)                                                    \
    do {                                                                       \
        if (!(test))                                                           \
            return code;                                                       \
    } while (0)
    a20_execution_domain_info_t domain;
    P_CHECK(a20_execution_domain_info(&domain) == A20_OK, 1);
    uint64_t stacks[2];
    a20_handle_t tasks[2];
    a20_handle_identity_t identities[2];
    for (unsigned i = 0; i < 2; i++) {
        P_CHECK(a20_vm_alloc_pages(4, 3, &stacks[i]) == A20_OK, 2);
        a20_thread_create_args_t args = {.entry =
                                             (uintptr_t)native_compute_worker,
                                         .arg = i,
                                         .stack_base = stacks[i] + 16384,
                                         .stack_size = 16384};
        P_CHECK(a20_thread_create(&args) >= 0, 3);
        tasks[i] = args.out_thread;
        P_CHECK(a20_hdl_identity(tasks[i], &identities[i]) == A20_OK &&
                    identities[i].state == A20_OBJECT_LIVE,
                4);
        a20_sched_args_t sched = {.flags = A20_SCHED_AFFINITY,
                                  .affinity = domain.online_mask,
                                  .affinity_size = sizeof(uint64_t)};
        P_CHECK(a20_sched_set(tasks[i], &sched) == A20_OK, 5);
    }
    for (unsigned i = 0; i < 2; i++) {
        uint32_t ready = 0;
        while (!ready) {
            P_CHECK(a20_atomic32_load(&native_compute_shared.ready[i],
                                      &ready) == A20_OK,
                    6);
            if (!ready) {
                a20_status_t s = a20_futex_wait(&native_compute_shared.ready[i],
                                                0, A20_TIMEOUT_INFINITE);
                P_CHECK(s == A20_OK || s == -A20_ERR_INTERRUPTED ||
                            s == -A20_ERR_WOULD_BLOCK,
                        7);
            }
        }
        P_CHECK(ready == 1, 7);
    }
    P_CHECK(a20_atomic32_store(&native_compute_shared.go, 1) == A20_OK, 8);
    uint32_t woken;
    P_CHECK(a20_futex_wake(&native_compute_shared.go, 2, &woken) == A20_OK, 9);
    /* This parent must resume even when every CPU is running a pure loop. */
    a20_time_t delay = {.secs = 0, .nsecs = 200000000};
    a20_status_t slept;
    do {
        slept = a20_thread_sleep(delay);
    } while (slept == -A20_ERR_INTERRUPTED);
    P_CHECK(slept == A20_OK, 10);
    for (unsigned i = 0; i < 2; i++) {
        P_CHECK(a20_task_wait(tasks[i], A20_TASK_WAIT_NONBLOCK, 0) ==
                    -A20_ERR_WOULD_BLOCK,
                11);
        P_CHECK(a20_task_kill(tasks[i], 9) == A20_OK, 12);
    }
    for (unsigned i = 0; i < 2; i++) {
        a20_task_status_t status;
        P_CHECK(a20_task_wait(tasks[i], 0, &status) == A20_OK &&
                    status.exit_code == 137,
                13);
        a20_handle_identity_t retired;
        P_CHECK(a20_hdl_identity(tasks[i], &retired) == A20_OK &&
                    retired.state == A20_OBJECT_RETIRED &&
                    a20_object_id_equal(&identities[i].id, &retired.id),
                14);
        P_CHECK(a20_hdl_close(tasks[i]) == A20_OK &&
                    a20_vm_unmap(stacks[i], 16384) == A20_OK,
                15);
    }
    return 0;
#undef P_CHECK
}
#endif
