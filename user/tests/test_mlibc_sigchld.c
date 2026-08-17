/*
 * SIGCHLD -> checkpoint delivery for forked native children.
 * A child exits; the parent must observe SIGCHLD via sigsuspend (the
 * mechanism mksh's job wait uses), then reap it.
 * Prints MLIBC_SIGCHLD: PASS on success.
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

static volatile sig_atomic_t g_sigchld = 0;

static void on_sigchld(int sig) {
	(void)sig;
	g_sigchld = 1;
}

int main(void) {
	struct sigaction sa = {};
	sa.sa_handler = on_sigchld;
	sigaction(SIGCHLD, &sa, NULL);

	pid_t pid = fork();
	if (pid < 0) {
		printf("MLIBC_SIGCHLD: FAIL fork errno=%d\n", errno);
		return 1;
	}
	if (pid == 0)
		_exit(7);

	/* Wait using the checkpoint model: sigsuspend with the default mask,
	 * like mksh's job wait loop. */
	sigset_t empty;
	sigemptyset(&empty);
	for (int i = 0; i < 200 && !g_sigchld; i++)
		sigsuspend(&empty);

	if (!g_sigchld) {
		printf("MLIBC_SIGCHLD: FAIL handler never ran\n");
		return 2;
	}
	int wstatus = 0;
	pid_t wp = waitpid(pid, &wstatus, 0);
	if (wp != pid || !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 7) {
		printf("MLIBC_SIGCHLD: FAIL waitpid wp=%d status=%x\n", (int)wp, wstatus);
		return 3;
	}
	printf("MLIBC_SIGCHLD: PASS\n");
	return 0;
}
