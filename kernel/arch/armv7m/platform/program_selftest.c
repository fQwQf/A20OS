#if defined(CONFIG_MCU_NATIVE_PROGRAM) && defined(CONFIG_MCU_SHARED_TEST)
#include "program.h"
#include "program_test.h"
#include "core/arch.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/panic.h"
#include "core/stdio.h"
#include "core/string.h"
#include "mm/slab.h"
#include "mm/elf.h"
#include "proc/proc.h"
#include "mcu/heap.h"
#include "sys/futex.h"

extern const uint8_t mcu_test_elf[], mcu_test_elf_end[];
static task_t *program_task;
static size_t program_heap_baseline;
unsigned program_test_done;
unsigned program_test_rejections;
#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x))                                                              \
            panic("program test: " #x);                                        \
    } while (0)

#include "program_app_test.h"
#include "program_file_test.h"

static void reject_image(const void *elf, size_t length) {
    CHECK(mcu_program_spawn(elf, length, 8192, NULL, 0, 2) == -ENOEXEC);
    program_test_rejections++;
}

static void controller(void) {
    armv7m_program_t *program = program_task->arch_program;
    mcu_program_test_record_t *record = program->startup;
    int pid = program_task->pid;
    for (;;) {
        CHECK(!record->error);
        if (record->phase == 1) {
            uint64_t flags = spin_lock_irqsave(&program_task->park_lock);
            int ready =
                program_task->state == PROC_BLOCKED && !program_task->on_cpu;
            spin_unlock_irqrestore(&program_task->park_lock, flags);
            if (ready)
                break;
        }
        proc_yield();
    }
    CHECK(record->threads_passed == 1 && record->thread_parent_cpu == 1 &&
          record->thread_worker_cpu == 0);
    CHECK(record->objects_passed == 1);
#ifdef CONFIG_NATIVE_FLAT_FILES
    CHECK(record->files_passed == 1 && record->spawn_passed == 1);
    CHECK(record->control_passed == 1);
    printf("[PROGRAM-TASK-CONTROL] PASS signal=12 masked=1 exit=137\n");
    printf("[PROGRAM-SPAWN] PASS children=2 exit=37 args=4 env=2 work=32\n");
    printf("[PROGRAM-FILES] PASS bytes=2048 fnv=60f4edc5 shared-native-io=1\n");
#endif
    CHECK(record->balance_mask == 3 && record->balance_jobs == 32 &&
          record->balance_sum == 4359168);
    printf("[PROGRAM-BALANCE] PASS jobs=32 sum=4359168 cpu-mask=3 unpinned=1 "
           "range=dynamic\n");
    printf("[PROGRAM-OBJECTS] PASS production directory real identities LIVE "
           "RETIRED revoke close=1\n");
    printf("[PROGRAM-RUNTIME] PASS immutable binding serial IDs survive join "
           "cross-cpu=1\n");
    printf("[PROGRAM-THREAD] PASS public affinity parent=1 child=0\n");
    printf("[PROGRAM-THREAD] PASS nested-create join close shared-space "
           "stack-pin=1\n");
    CHECK(record->allocator_passed == 1);
    printf("[PROGRAM-ALLOCATOR] PASS common SDK malloc/calloc/realloc/free\n");
    CHECK(record->heap_checks == 20);
    CHECK(a20_flat_unmap(&program->owner->memory, record->heap_base, 8192) ==
          -EBUSY);
    CHECK(a20_flat_destroy(&program->owner->memory) == -EBUSY);
    uint32_t *word = (void *)(uintptr_t)record->heap_base;
    printf("[PROGRAM-HEAP] PASS checks=%u wait-pinned=1 cross-cpu=1\n",
           record->heap_checks);
    proc_sched_config_t config = {.fields = PROC_SCHED_AFFINITY, .affinity = 1};
    CHECK(proc_sched_set(program_task, &config) == 0);
    __atomic_store_n(word, 1, __ATOMIC_RELEASE);
    record->phase = 2;
    CHECK(futex_wake_user((int *)word, 1) == 1);
    while (record->phase != 4) {
        CHECK(!record->error);
        proc_yield();
    }
    /* Race a wake against wait admission: before linkage gives WOULD_BLOCK,
     * after linkage wakes the waiter. Either ordering must make progress. */
    __atomic_store_n(word, 2, __ATOMIC_RELEASE);
    int woke = futex_wake_user((int *)word, 1);
    CHECK(woke == 0 || woke == 1);
    while (record->phase != 3) {
        CHECK(!record->error);
        proc_yield();
    }
    CHECK(!record->error && record->cpu_before == 1 && record->cpu_after == 0);
    CHECK(record->domain_mask_before == 3 && record->domain_mask_after == 3);
    printf("[PROGRAM-DOMAIN] PASS same kernel before/after CPU migration "
           "mask=3\n");
    CHECK(record->stack_before == record->stack_after &&
          record->stack_before >= 0x60000000U);
    uintptr_t stack = record->stack_after;
    CHECK(record->futex_checks == 12 && record->futex_waits >= 1);
    CHECK(futex_wake_user((int *)word, 1) == 0);
    printf(
        "[PROGRAM-FUTEX] PASS checks=%u waits=%u remote-wake=1 race-wake=%d\n",
        record->futex_checks, record->futex_waits, woke);
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&program_task->park_lock);
        int exited = (program_task->state == PROC_ZOMBIE ||
                      program_task->state == PROC_UNUSED) &&
                     !program_task->on_cpu;
        spin_unlock_irqrestore(&program_task->park_lock, flags);
        if (exited)
            break;
        proc_yield();
    }
    CHECK(program_task->pid == pid && program_task->exit_code == 0);
    __atomic_store_n(&record->parent_exited, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&record->orphan_done, __ATOMIC_ACQUIRE)) {
        CHECK(!record->error);
        proc_yield();
    }
    printf("[PROGRAM-THREAD] PASS child survives leader exit image heap "
           "startup=live\n");
    app_test_finished();
    while (__atomic_load_n(&mcu_program_live, __ATOMIC_ACQUIRE) ||
           mcu_heap_available() != program_heap_baseline)
        proc_yield();
    printf("[PROGRAM-THREAD] PASS final heap restored handles stacks "
           "image=freed\n");
    printf("[PROGRAM-TEST] PASS pid=%d migrated=1->0 stack=0x%x ELF=ARM32-PIE "
           "SVC=native released=all\n",
           pid, (unsigned)stack);
    __atomic_store_n(&program_test_done, 1, __ATOMIC_RELEASE);
    proc_exit(0);
}

void mcu_program_selftest_start(void) {
    size_t length = mcu_test_elf_end - mcu_test_elf;
    size_t before = mcu_heap_available();
    uint8_t *bad = kmalloc(length);
    CHECK(bad);
    memcpy(bad, mcu_test_elf, length);
    bad[0] = 0;
    reject_image(bad, length);
    memcpy(bad, mcu_test_elf, length);
    ((Elf32_Ehdr *)bad)->e_phoff = 0xfffffff0U;
    reject_image(bad, length);
    reject_image(mcu_test_elf, 8);
    memcpy(bad, mcu_test_elf, length);
    ((Elf32_Ehdr *)bad)->e_entry &= ~1U;
    reject_image(bad, length);
    memcpy(bad, mcu_test_elf, length);
    Elf32_Phdr *ph = (void *)(bad + ((Elf32_Ehdr *)bad)->e_phoff);
    CHECK(ph[0].p_type == PT_LOAD);
    ph[0].p_memsz = ph[0].p_filesz - 1;
    reject_image(bad, length);
    memcpy(bad, mcu_test_elf, length);
    ph[0].p_align = 3;
    reject_image(bad, length);
    memcpy(bad, mcu_test_elf, length);
    Elf32_Dyn *dynamic = NULL;
    for (unsigned i = 0; i < ((Elf32_Ehdr *)bad)->e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC)
            dynamic = (void *)(bad + ph[i].p_offset);
    CHECK(dynamic);
    dynamic[0].d_tag = 1; /* DT_NEEDED: imports are not implemented. */
    reject_image(bad, length);
    memcpy(bad, mcu_test_elf, length);
    uint32_t rel = 0;
    for (Elf32_Dyn *d = dynamic; d->d_tag != DT_NULL; d++)
        if (d->d_tag == DT_REL)
            rel = d->d_un;
    CHECK(rel);
    uint32_t *relocation = NULL;
    for (unsigned i = 0; i < ((Elf32_Ehdr *)bad)->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && rel >= ph[i].p_vaddr &&
            rel - ph[i].p_vaddr < ph[i].p_filesz)
            relocation = (void *)(bad + ph[i].p_offset + rel - ph[i].p_vaddr);
    CHECK(relocation);
    relocation[1] =
        2; /* R_ARM_ABS32: never silently accept unsupported types. */
    reject_image(bad, length);
    kfree(bad);
    CHECK(mcu_heap_available() == before && !mcu_program_live);
    int controller_pid = proc_alloc_deferred(controller);
    CHECK(controller_pid > 0);
    task_t *control = proc_find_get(controller_pid);
    CHECK(control);
    app_test_init(control, length);
#ifdef CONFIG_NATIVE_FLAT_FILES
    program_file_test_bind();
#endif
    program_heap_baseline = mcu_heap_available();
    app_test_launch();
    proc_publish_deferred_task(control);
    proc_put(control);
}
#endif
