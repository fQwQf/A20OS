/*
 * Pipe + fork + EOF on the Native ABI: the child writes to a pipe, the
 * parent reads until EOF.  Verifies the channel-backed pipe signals
 * peer_closed when the writer exits.
 * Prints MLIBC_PIPE: PASS on success.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

int main(void) {
	int pfd[2];
	if (pipe(pfd) != 0) {
		printf("MLIBC_PIPE: FAIL pipe errno=%d\n", errno);
		return 1;
	}
	pid_t pid = fork();
	if (pid < 0) {
		printf("MLIBC_PIPE: FAIL fork errno=%d\n", errno);
		return 2;
	}
	if (pid == 0) {
		close(pfd[0]);
		if (write(pfd[1], "pipe-data", 9) != 9) {
			_exit(3);
		}
		close(pfd[1]);
		_exit(0);
	}
	close(pfd[1]);
	char buf[64] = {0};
	ssize_t n = 0, total = 0;
	while (total < (ssize_t)sizeof(buf)) {
		n = read(pfd[0], buf + total, sizeof(buf) - total - 1);
		if (n == 0)
			break;
		if (n < 0) {
			printf("MLIBC_PIPE: FAIL read errno=%d\n", errno);
			return 4;
		}
		total += n;
	}
	if (total != 9 || memcmp(buf, "pipe-data", 9) != 0) {
		printf("MLIBC_PIPE: FAIL data total=%d\n", (int)total);
		return 5;
	}
	int st = 0;
	if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		printf("MLIBC_PIPE: FAIL wait st=%x\n", st);
		return 6;
	}
	printf("MLIBC_PIPE: PASS\n");
	return 0;
}
