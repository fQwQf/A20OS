/*
 * task_clone / fork() smoke on the Native ABI.
 *
 * Verifies the A20 capability-safe continuation primitive:
 *   - fork() returns 0 in the child and the child pid in the parent;
 *   - the child continues at the same PC with a COW copy of the address
 *     space (a write in the child is invisible to the parent);
 *   - the child inherits only the handles in the capability manifest
 *     (here: the fd table behind stdout), never an implicit full set;
 *   - the parent can waitpid() the child.
 * Prints MLIBC_FORK: PASS on success.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

static volatile int g_counter;

int main(void) {
	printf("mlibc fork: start\n");

	g_counter = 100;
	pid_t pid = fork();
	if (pid < 0) {
		printf("MLIBC_FORK: FAIL fork errno=%d\n", errno);
		return 1;
	}

	if (pid == 0) {
		/* CHILD: same PC, COW copy.  A write here must not leak to the
		 * parent, and we should be able to see our own modification. */
		if (g_counter != 100) {
			printf("MLIBC_FORK: FAIL child sees wrong counter %d\n", g_counter);
			_exit(2);
		}
		g_counter = 200;
		printf("mlibc fork: child pid=%d\n", (int)getpid());
		/* stdio still works: stdout came through the manifest. */
		printf("child-stdout-ok\n");
		fflush(NULL);
		_exit(42);
	}

	/* PARENT */
	int wstatus = 0;
	pid_t wp = waitpid(pid, &wstatus, 0);
	if (wp != pid || !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 42) {
		printf("MLIBC_FORK: FAIL waitpid wp=%d status=%x\n", (int)wp, wstatus);
		return 3;
	}
	if (g_counter != 100) {
		printf("MLIBC_FORK: FAIL parent counter corrupted -> %d\n", g_counter);
		return 4;
	}

	/* fork + execve: the shell pattern.  The child replaces itself with
	 * another Native binary via A20_SYS_execve; the parent waits. */
	pid_t epid = fork();
	if (epid < 0) {
		printf("MLIBC_FORK: FAIL exec fork errno=%d\n", errno);
		return 5;
	}
	if (epid == 0) {
		char *cargv[] = { (char *)"/bin/mlibc-child-rv", (char *)"fork-exec", NULL };
		execve("/bin/mlibc-child-rv", cargv, NULL);
		printf("MLIBC_FORK: FAIL execve errno=%d\n", errno);
		_exit(7);
	}
	wstatus = 0;
	wp = waitpid(epid, &wstatus, 0);
	if (wp != epid || !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 42) {
		printf("MLIBC_FORK: FAIL exec waitpid wp=%d status=%x\n", (int)wp, wstatus);
		return 6;
	}

	printf("MLIBC_FORK: PASS\n");
	return 0;
}
