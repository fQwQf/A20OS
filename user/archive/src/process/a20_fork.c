/*
 * musl A20 fork — not supported (use task_spawn instead).
 */
#include <errno.h>

long __sys_fork(void) { return -ENOSYS; }
long __sys_vfork(void) { return -ENOSYS; }
