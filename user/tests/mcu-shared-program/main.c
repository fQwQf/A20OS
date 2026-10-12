#include "a20_task.h"
#include "a20_execution.h"
#include "a20_sync.h"
#include "a20_mem.h"
#include "program_test.h"
#include "../native-memory-contract.h"
#include "../native-domain-contract.h"
#include "../native-thread-contract.h"
#ifdef A20_NATIVE_FILE_TEST
#include "../native-file-contract.h"
#define A20_SPAWN_FLAT_TEST 1
#include "../native-spawn-contract.h"
#endif

static const uint32_t magic[2] = {0xa20beef, 0xc0ffee};
static const uint32_t *volatile relocated = magic;
static uint32_t zero_bss[4];

static __attribute__((noreturn)) void orphan_entry(uintptr_t arg) {
    mcu_program_test_record_t *record = (void *)arg;
    uint32_t ready = 0;
    do {
        if (a20_atomic32_load((uint32_t *)&record->parent_exited, &ready))
            a20_thread_exit(99);
        if (!ready)
            a20_thread_yield();
    } while (!ready);
    a20_execution_domain_info_t domain;
    uint64_t page = 0;
    if (relocated[0] != 0xa20beef || zero_bss[3] != 99 ||
        native_domain_contract(&domain) || !native_runtime_matches() ||
        a20_vm_alloc_pages(1, 3, &page) || a20_vm_unmap(page, 4096)) {
        record->error = 87;
        a20_thread_exit(87);
    }
    a20_atomic32_store((uint32_t *)&record->orphan_done, 1);
    a20_thread_exit(0);
    __builtin_unreachable();
}

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
                   A20_EXEC_FUTEX | A20_EXEC_ATOMIC32 | A20_EXEC_USER_HEAP |
                   A20_EXEC_THREAD_CREATE) &&
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
    a20_execution_domain_info_t domain;
    CHECK(native_domain_contract(&domain) == 0 && domain.cpu_online == 2 &&
              domain.online_mask == 3,
          82);
    record->domain_mask_before = domain.online_mask;
    CHECK(native_runtime_contract() == 0, 85);
    int thread_result = native_thread_contract();
    record->balance_mask = native_balance_mask();
    record->balance_jobs =
        native_balance_shared.jobs[0] + native_balance_shared.jobs[1];
    record->balance_sum =
        native_balance_shared.sum[0] + native_balance_shared.sum[1];
    CHECK(thread_result == 0, 84);
#ifdef A20_NATIVE_FILE_TEST
    CHECK(native_file_contract() == 0 && native_file_reopen(17) == 0, 89);
    record->files_passed = 1;
    CHECK(native_spawn_contract() == 0, 88);
    record->spawn_passed = 1;
#endif
    record->threads_passed = 1;
    record->objects_passed = native_object_releases == 1 &&
                             !native_object_close_error &&
                             !native_object_record[0].publication;
    record->thread_parent_cpu = native_thread_shared.parent_cpu;
    record->thread_worker_cpu = native_thread_shared.worker_cpu;
    CHECK(native_memory_contract() == 0, 81);
    record->allocator_passed = 1;
    uint64_t heap = 0, temp = 0;
#define HCHECK(test, code)                                                     \
    do {                                                                       \
        CHECK(test, code);                                                     \
        record->heap_checks++;                                                 \
    } while (0)
    HCHECK(a20_vm_alloc_pages(2, 3, &heap) == A20_OK && !(heap & 4095), 60);
    uint32_t *heap_words = (void *)(uintptr_t)heap;
    for (unsigned i = 0; i < 8192 / sizeof(*heap_words); i++)
        CHECK(heap_words[i] == 0, 61);
    HCHECK(a20_atomic32_store(heap_words + 2047, 0xdeadbeef) == A20_OK, 62);
    uint32_t value;
    HCHECK(a20_atomic32_load(heap_words + 2047, &value) == A20_OK &&
               value == 0xdeadbeef,
           63);
    HCHECK(a20_atomic32_load(heap_words + 2048, &value) == -A20_ERR_FAULT, 64);
    HCHECK(a20_vm_unmap(heap + 4096, 4096) == -A20_ERR_INVALID_ARGUMENT, 65);
    HCHECK(a20_vm_unmap(heap, 0) == -A20_ERR_INVALID_ARGUMENT, 66);
    a20_vm_alloc_args_t bad_alloc = {
        .size = sizeof(bad_alloc), .version = 1, .prot = 3};
    HCHECK(a20_vm_alloc(&bad_alloc) == -A20_ERR_INVALID_ARGUMENT, 67);
    bad_alloc.length = UINT64_MAX;
    HCHECK(a20_vm_alloc(&bad_alloc) == -A20_ERR_INVALID_ARGUMENT, 68);
    bad_alloc.length = 4096;
    bad_alloc.prot = 1;
    HCHECK(a20_vm_alloc(&bad_alloc) == -A20_ERR_INVALID_ARGUMENT, 69);
    bad_alloc.prot = 3;
    bad_alloc.flags = 1;
    HCHECK(a20_vm_alloc(&bad_alloc) == -A20_ERR_INVALID_ARGUMENT, 70);
    bad_alloc.flags = 0;
    bad_alloc.addr_hint = heap;
    HCHECK(a20_vm_alloc(&bad_alloc) == -A20_ERR_INVALID_ARGUMENT, 71);
    bad_alloc.addr_hint = 0;
    bad_alloc.size--;
    HCHECK(a20_vm_alloc(&bad_alloc) == -A20_ERR_INVALID_ARGUMENT, 72);
    HCHECK(a20_vm_unmap(heap, (1ULL << 32) | 8192) == -A20_ERR_INVALID_ARGUMENT,
           79);
    HCHECK(a20_vm_alloc_pages((UINT64_MAX / 4096) + 1, 3, &temp) ==
               -A20_ERR_INVALID_ARGUMENT,
           80);
    HCHECK(a20_vm_alloc_pages(512, 3, &temp) == -A20_ERR_NO_MEMORY, 73);
    HCHECK(a20_vm_alloc_pages(1, 3, &temp) == A20_OK, 74);
    HCHECK(a20_futex_wait((void *)(uintptr_t)temp, 0, 3000000) ==
               -A20_ERR_TIMED_OUT,
           75);
    HCHECK(a20_vm_unmap(temp, 4096) == A20_OK, 76);
    HCHECK(a20_atomic32_load((void *)(uintptr_t)temp, &value) == -A20_ERR_FAULT,
           77);
    HCHECK(a20_vm_alloc_pages(1, 3, &temp) == A20_OK, 78);
    /* Keep this allocation and the wait word live: final task release must
     * reclaim them without relying on a cooperative user free call. */
    record->heap_base = (uint32_t)heap;
#undef HCHECK
    uint32_t *word = heap_words;
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
    CHECK(native_domain_contract(&domain) == 0, 83);
    record->domain_mask_after = domain.online_mask;
    uint64_t orphan_stack = 0;
    CHECK(a20_vm_alloc_pages(4, 3, &orphan_stack) == A20_OK, 85);
    a20_thread_create_args_t orphan = {.entry = (uintptr_t)orphan_entry,
                                       .arg = (uintptr_t)record,
                                       .stack_base = orphan_stack + 16384,
                                       .stack_size = 16384};
    CHECK(a20_thread_create(&orphan) >= 0, 86);
    record->phase = 3;
    /* Leave a live thread, its stack and handle to the shared finalizer. */
    a20_thread_exit(0);
    __builtin_unreachable();
}
