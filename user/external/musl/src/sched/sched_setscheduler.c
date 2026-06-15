#include <sched.h>
#include <errno.h>
#include "syscall.h"

int sched_setscheduler(pid_t pid, int sched, const struct sched_param *param)
{
	return syscall(SYS_sched_setscheduler, pid, sched, param);
}
