/*
 * Freestanding startup for the A20OS MCU learner demos.
 *
 * The loader places the run record on this thread's stack and passes its
 * address in a0; no libc, no TLS and no dynamic linker are involved.
 */
#include "a20_task.h"
#include "a20_mcu_demo.h"

extern int demo_main(a20_mcu_demo_record_t *record);

__attribute__((noreturn)) void _start(void *startup)
{
    int status = demo_main((a20_mcu_demo_record_t *)startup);
    a20_thread_exit(status);
    for (;;) {
    }
}
