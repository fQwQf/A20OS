#include "core/panic.h"
#include "fs/fdtable.h"
#include "proc/proc.h"

void *a20_fdtable_get_files(task_t *task)
{
    return task ? task->files : NULL;
}

void a20_fdtable_set_files(task_t *task, void *files)
{
    if (task)
        task->files = files;
}

int a20_fdtable_fd_limit(task_t *task)
{
    if (!task)
        return MAX_FILES;
    uint64_t limit = task->limits.nofile;
    if (limit > MAX_FILES)
        limit = MAX_FILES;
    return (int)limit;
}

void a20_fdtable_panic_oom(void)
{
    panic("fdtable: no memory");
}
