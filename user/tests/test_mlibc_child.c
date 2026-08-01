/* mlibc-on-A20 spawn test child: prints argv/env info and exits with 42. */
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

static volatile int g_got_sigusr1 = 0;

static void sigusr1_handler(int sig) {
	(void)sig;
	g_got_sigusr1 = 1;
	printf("mlibc child: SIGUSR1 handler ran\n");
	fflush(stdout);
}

int main(int argc, char **argv) {
	printf("mlibc child: alive argc=%d\n", argc);
	if (argc > 1)
		printf("mlibc child: argv1=%s\n", argv[1]);
	char cwd[128];
	if (getcwd(cwd, sizeof(cwd)))
		printf("mlibc child: cwd=%s\n", cwd);
	if (argc > 1 && strcmp(argv[1], "cwd-spawn") == 0) {
		if (strcmp(cwd, "/bin") != 0)
			return 44;
		int fd = open("sh", O_RDONLY);
		printf("mlibc child: relative-open=%s\n", fd >= 0 ? "ok" : "fail");
		if (fd >= 0)
			close(fd);
		if (fd < 0)
			return 43;
	}
	if (argc > 1 && strcmp(argv[1], "fd-actions") == 0) {
		if (fcntl(3, F_GETFD) != -1 || errno != EBADF)
			return 45;
		if (fcntl(4, F_GETFD) < 0)
			return 46;
		char buf[8] = {0};
		if (read(4, buf, 6) != 6 || memcmp(buf, "hello\n", 6) != 0)
			return 47;
	}
	if (argc > 1 && strcmp(argv[1], "signal-spawn") == 0) {
		/* Register a SIGUSR1 handler, then sleep at a checkpoint.  The
		 * parent sends SIGUSR1; the handler runs at the sleep checkpoint
		 * and the child reports it via its exit status. */
		struct sigaction sa;
		sa.sa_handler = sigusr1_handler;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		if (sigaction(SIGUSR1, &sa, NULL) != 0)
			return 50;
		printf("mlibc child: signal handler ready\n");
		fflush(stdout);
		for (int i = 0; i < 20 && !g_got_sigusr1; i++)
			usleep(10000);   /* checkpoint: futex/nanosleep */
		if (!g_got_sigusr1)
			return 51;
		return 77;          /* SIGUSR1 handled */
	}
	return 42;
}
