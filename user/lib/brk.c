#define _BSD_SOURCE
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <sys/syscall.h>

int brk(void *end)
{
	uintptr_t new = (uintptr_t)end;
	uintptr_t cur = (uintptr_t)syscall(SYS_brk, new);
	if (new && cur < new) {
		errno = ENOMEM;
		return -1;
	}
	return 0;
}

void *sbrk(intptr_t inc)
{
	uintptr_t cur = (uintptr_t)syscall(SYS_brk, 0);
	if (inc) {
		uintptr_t new = (uintptr_t)syscall(SYS_brk, cur + inc);
		if (new < cur + inc) {
			errno = ENOMEM;
			return (void *)-1;
		}
	}
	return (void *)cur;
}
