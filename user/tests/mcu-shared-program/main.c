#include "a20_task.h"
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
    CHECK(a20_thread_get_cpu(&record->cpu_before) == 0 &&
              record->cpu_before == 1,
          3);
    CHECK(a20_thread_get_cpu((uint32_t *)0x20000000UL) == -A20_ERR_FAULT, 4);
    CHECK(a20_syscall6(0x7fff, 0, 0, 0, 0, 0, 0) == -A20_ERR_NOT_SUPPORTED, 5);
    record->stack_before = (uintptr_t)&local[0];
    CHECK(a20_thread_yield() == 0, 6);
    record->phase = 1;
    while (record->phase != 2)
        CHECK(a20_thread_sleep((a20_time_t){.nsecs = 100000000}) == 0, 6);
    CHECK(a20_thread_get_cpu(&record->cpu_after) == 0 && record->cpu_after == 0,
          7);
    CHECK(a20_thread_sleep((a20_time_t){.nsecs = 3000000}) == 0, 8);
    record->stack_after = (uintptr_t)&local[0];
    CHECK(record->stack_before == record->stack_after && local[0] == 11 &&
              local[3] == 44,
          9);
    CHECK(zero_bss[3] == 99 && relocated[1] == 0xc0ffee, 10);
    record->phase = 3;
    a20_task_exit(0);
    __builtin_unreachable();
}
