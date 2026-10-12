#include "a20_task.h"
#include "a20_execution.h"
#include "a20_sync.h"
#include "program_test.h"

static const uint32_t magic[2] = {0xa20beef, 0xc0ffee};
static const uint32_t *volatile relocated = magic;
static uint32_t zero_bss[4];

__attribute__((noreturn)) void _start(mcu_program_test_record_t *record) {
    volatile uint32_t local[4] = {11, 22, 33, 44};
#define CHECK(test, code)                                                      \
    do {                                                                       \
        if (!(test)) {                                                         \
            record->error = code;                                              \
            record->phase = 3;                                                 \
            a20_task_exit(code);                                               \
        }                                                                      \
    } while (0)
    CHECK(relocated[0] == 0xa20beef && relocated[1] == 0xc0ffee, 1);
    CHECK(zero_bss[0] == 0 && zero_bss[3] == 0, 2);
    zero_bss[3] = 99;
    a20_execution_info_t execution;
    CHECK(a20_execution_info(&execution) == A20_OK &&
              a20_execution_valid(&execution) && execution.pointer_bits == 32 &&
              execution.elf_machine == 40 &&
              execution.calling_abi == A20_EXEC_ABI_THUMB_SOFT &&
              execution.features ==
                  (A20_EXEC_THREAD_BASIC | A20_EXEC_NATIVE_IMAGE |
                   A20_EXEC_FUTEX | A20_EXEC_ATOMIC32) &&
              execution.memory_model == A20_EXEC_MEMORY_FLAT &&
              execution.atomic_model == A20_EXEC_ATOMIC_FABRIC_GATE,
          11);
    CHECK(a20_syscall6(A20_SYS_execution_info, (uintptr_t)&execution,
                       sizeof(execution) - 1, 0, 0, 0,
                       0) == -A20_ERR_INVALID_ARGUMENT,
          12);
    CHECK(a20_execution_info((void *)0x20000000UL) == -A20_ERR_FAULT, 13);

    CHECK(a20_thread_get_cpu(&record->cpu_before) == 0 &&
              record->cpu_before == 1,
          3);
    CHECK(a20_thread_get_cpu((uint32_t *)0x20000000UL) == -A20_ERR_FAULT, 4);
    CHECK(a20_syscall6(0x7fff, 0, 0, 0, 0, 0, 0) == -A20_ERR_NOT_SUPPORTED, 5);
    record->stack_before = (uintptr_t)&local[0];
    CHECK(a20_thread_yield() == 0, 6);
    uint32_t *word = &record->futex_word;
    CHECK(a20_futex_wait(word, 1, A20_TIMEOUT_INFINITE) == -A20_ERR_WOULD_BLOCK,
          20);
    CHECK(a20_futex_wait(word, 0, 3000000) == -A20_ERR_TIMED_OUT, 21);
    CHECK(a20_futex_wait((void *)0x20000000UL, 0, 3000000) == -A20_ERR_FAULT,
          22);
    CHECK(a20_futex_wait((void *)((uintptr_t)word + 1), 0, 3000000) ==
              -A20_ERR_INVALID_ARGUMENT,
          23);
    a20_futex_wait_args_t bad_wait = {.size = sizeof(bad_wait),
                                      .version = 1,
                                      .addr = (1ULL << 32) | (uintptr_t)word,
                                      .timeout_ns = 3000000};
    CHECK(a20_syscall6(A20_SYS_futex_wait, (uintptr_t)&bad_wait, 0, 0, 0, 0,
                       0) == -A20_ERR_FAULT,
          24);
    bad_wait.addr = (uintptr_t)word;
    bad_wait.flags = 1;
    CHECK(a20_syscall6(A20_SYS_futex_wait, (uintptr_t)&bad_wait, 0, 0, 0, 0,
                       0) == -A20_ERR_INVALID_ARGUMENT,
          25);
    bad_wait.flags = 0;
    bad_wait.size--;
    CHECK(a20_syscall6(A20_SYS_futex_wait, (uintptr_t)&bad_wait, 0, 0, 0, 0,
                       0) == -A20_ERR_INVALID_ARGUMENT,
          26);
    uint32_t woken = 99;
    CHECK(a20_futex_wake(word, 1, &woken) == A20_OK && woken == 0, 27);
    CHECK(a20_futex_wake(word, 0, &woken) == -A20_ERR_INVALID_ARGUMENT, 28);
    CHECK(a20_futex_wake((void *)0x20000000UL, 1, &woken) == -A20_ERR_FAULT,
          29);
    a20_futex_wake_args_t bad_wake = {.size = sizeof(bad_wake),
                                      .version = 1,
                                      .addr = (uintptr_t)word,
                                      .count = 1,
                                      .reserved = 1};
    CHECK(a20_syscall6(A20_SYS_futex_wake, (uintptr_t)&bad_wake, 0, 0, 0, 0,
                       0) == -A20_ERR_INVALID_ARGUMENT,
          30);
    bad_wake.reserved = 0;
    bad_wake.addr |= 1ULL << 32;
    CHECK(a20_syscall6(A20_SYS_futex_wake, (uintptr_t)&bad_wake, 0, 0, 0, 0,
                       0) == -A20_ERR_FAULT,
          31);
    record->futex_checks = 12;
    uint32_t observed, exchanged;
    CHECK(a20_atomic32_load(word, &observed) == A20_OK && observed == 0, 40);
    CHECK(a20_atomic32_store(word, 0xaabbccddU) == A20_OK, 41);
    CHECK(a20_atomic32(word, A20_ATOMIC32_COMPARE_EXCHANGE, 7, 8, &observed,
                       &exchanged) == A20_OK &&
              observed == 0xaabbccddU && exchanged == 0,
          42);
    CHECK(a20_atomic32(word, A20_ATOMIC32_COMPARE_EXCHANGE, 0xaabbccddU, 9,
                       &observed, &exchanged) == A20_OK &&
              observed == 0xaabbccddU && exchanged == 1,
          43);
    CHECK(a20_atomic32(word, A20_ATOMIC32_EXCHANGE, 0, 0, &observed, 0) ==
                  A20_OK &&
              observed == 9,
          44);
    CHECK(a20_atomic32((void *)0x20000000UL, A20_ATOMIC32_LOAD, 0, 0, &observed,
                       0) == -A20_ERR_FAULT,
          45);
    CHECK(a20_atomic32((void *)((uintptr_t)word + 1), A20_ATOMIC32_LOAD, 0, 0,
                       &observed, 0) == -A20_ERR_INVALID_ARGUMENT,
          46);
    CHECK(a20_atomic32(word, 99, 0, 0, &observed, 0) ==
              -A20_ERR_INVALID_ARGUMENT,
          47);
    record->phase = 1;
    CHECK(a20_futex_wait(word, 0, UINT64_MAX - 1) == A20_OK &&
              a20_atomic32_load(word, &observed) == A20_OK && observed == 1,
          32);
    record->futex_waits++;
    CHECK(record->phase == 2, 33);
    CHECK(a20_thread_get_cpu(&record->cpu_after) == 0 && record->cpu_after == 0,
          7);
    CHECK(a20_thread_sleep((a20_time_t){.nsecs = 3000000}) == 0, 8);
    record->stack_after = (uintptr_t)&local[0];
    CHECK(record->stack_before == record->stack_after && local[0] == 11 &&
              local[3] == 44,
          9);
    CHECK(zero_bss[3] == 99 && relocated[1] == 0xc0ffee, 10);
    record->phase = 4;
    for (;;) {
        CHECK(a20_atomic32_load(word, &observed) == A20_OK, 48);
        if (observed != 1)
            break;
        a20_status_t st = a20_futex_wait(word, 1, A20_TIMEOUT_INFINITE);
        CHECK(st == A20_OK || st == -A20_ERR_WOULD_BLOCK, 34);
        record->futex_waits++;
    }
    CHECK(observed == 2, 35);
    record->phase = 3;
    a20_task_exit(0);
    __builtin_unreachable();
}
