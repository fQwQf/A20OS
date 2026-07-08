#include "core/types.h"
#include "proc/proc.h"
#include "abi/linux/syscall_impl.h"
#include "trap_frame.h"

int64_t sys_set_thread_area(void *ptr)
{
    task_t *cur = proc_current();
    if (cur && cur->trap_ctx)
        TRAP_CTX_TP(cur->trap_ctx) = (uint32_t)(uintptr_t)ptr;

    __asm__ __volatile__ (
        "mcr p15, 0, %0, c13, c0, 3"
        :: "r"(ptr)
    );
    return 0;
}
