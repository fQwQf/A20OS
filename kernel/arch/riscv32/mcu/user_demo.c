/*
 * Launcher for the LiteOS experiments on the RV32 MCU port.
 *
 * The image is embedded in the kernel (demo_image.S), loaded into an owned
 * flat space and started in U-mode.  This task only drains the run record:
 * the demo itself has no console API, so what is printed here is exactly the
 * evidence the user program produced.
 */
#if defined(CONFIG_MCU_USER_DEMO) && defined(CONFIG_MCU_NATIVE_PROGRAM)
#include "core/arch.h"
#include "core/panic.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "program.h"
#include "uapi/a20/a20_mcu_demo.h"
#include "mcu/heap.h"

extern const uint8_t mcu_demo_elf[], mcu_demo_elf_end[];

static void print_line(a20_mcu_demo_record_t *r)
{
    unsigned slot = r->tail % A20_MCU_DEMO_LINES;
    r->lines[slot][A20_MCU_DEMO_LINE - 1] = '\0';
    printf("%s\n", r->lines[slot]);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    r->tail = r->tail + 1;
}

static void demo_runner(void)
{
    /* Static, not stack: the record is larger than this task's kernel stack
     * and it must outlive every trap that delivers the demo's output. */
    static a20_mcu_demo_record_t record;
    __builtin_memset(&record, 0, sizeof(record));
    record.magic = A20_MCU_DEMO_MAGIC;
    record.version = A20_MCU_DEMO_VERSION;

    int pid = mcu_program_spawn(mcu_demo_elf, (size_t)(mcu_demo_elf_end - mcu_demo_elf),
                                8192, &record, sizeof(record), 1);
    if (pid < 0) {
        printf("[DEMO] spawn failed: %d\n", pid);
        return;
    }
    task_t *task = proc_find_get(pid);
    if (!task || !task->arch_program) {
        printf("[DEMO] task disappeared\n");
        return;
    }
    a20_mcu_demo_record_t *r = task->arch_program->startup;

    for (;;) {
        while (r->tail != r->head)
            print_line(r);
        if (r->done)
            break;
        proc_sleep_until(timer_get_ticks() + 1U);
    }
    while (r->tail != r->head)
        print_line(r);
    printf("[DEMO] experiments 4.1-4.9: %s (checks=%u, heap-free=%u)\n",
           r->status == 0 ? "PASS" : "FAIL", (unsigned)r->checks,
           (unsigned)mcu_heap_available());
    proc_put(task);
}

void mcu_user_demo_start(void)
{
    if (proc_alloc(demo_runner) < 0)
        panic("MCU user demo: cannot allocate task");
}
#endif
