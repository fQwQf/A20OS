#include <proc/proc.h>
#include <core/cpu.h>
#include <core/klog.h>

task_t *a20_task_all_next(task_t *t)
{
    return t ? t->all_next : NULL;
}

void a20_task_set_all_next(task_t *t, task_t *n)
{
    if (t)
        t->all_next = n;
}

task_t *a20_task_all_prev(task_t *t)
{
    return t ? t->all_prev : NULL;
}

void a20_task_set_all_prev(task_t *t, task_t *p)
{
    if (t)
        t->all_prev = p;
}

int a20_task_pid(task_t *t)
{
    return t ? t->pid : -1;
}

int a20_arch_is_kernel_address(uintptr_t addr)
{
    return arch_is_kernel_address((const void *)addr);
}

void a20_proc_list_corrupt(int pid, void *ptr)
{
    kerr("proc_next_task_locked: corrupt all_next from pid=%d ptr=%p\n", pid, ptr);
}
