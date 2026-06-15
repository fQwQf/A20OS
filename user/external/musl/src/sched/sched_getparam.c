#include <sched.h>
#include <errno.h>
#include "syscall.h"

int sched_getparam(pid_t pid, struct sched_param *param)
{
	return syscall(SYS_sched_getparam, pid, param);
}
