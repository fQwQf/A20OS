/*
 * mlibc-on-A20 hello world: stdio, malloc, file I/O, threads+futex,
 * posix_spawn/waitpid, pipe, poll, socketpair.
 * Prints MLIBC_A20: PASS on success, FAIL with a subsystem code otherwise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <spawn.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <errno.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_counter;
static int g_seen[4];

static void *worker(void *arg) {
	int id = (int)(size_t)arg;
	for (int i = 0; i < 100; i++) {
		pthread_mutex_lock(&g_lock);
		g_counter++;
		g_seen[id] = 1;
		pthread_mutex_unlock(&g_lock);
	}
	return NULL;
}

int main(void) {
	printf("mlibc on A20: stdio works\n");

	char *p = malloc(4096);
	if (!p) {
		printf("MLIBC_A20: FAIL malloc\n");
		return 1;
	}
	memset(p, 0x5a, 4096);
	free(p);

	int fd = open("/mlibc_test.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		printf("MLIBC_A20: FAIL open\n");
		return 2;
	}
	const char msg[] = "a20 file io";
	if (write(fd, msg, sizeof(msg) - 1) != (ssize_t)(sizeof(msg) - 1)) {
		printf("MLIBC_A20: FAIL write\n");
		return 3;
	}
	if (lseek(fd, 0, SEEK_SET) != 0) {
		printf("MLIBC_A20: FAIL lseek\n");
		return 4;
	}
	char buf[32] = {0};
	if (read(fd, buf, sizeof(msg) - 1) != (ssize_t)(sizeof(msg) - 1) ||
	    strcmp(buf, msg) != 0) {
		printf("MLIBC_A20: FAIL read\n");
		return 5;
	}
	close(fd);

	struct stat st;
	if (stat("/mlibc_test.txt", &st) != 0 || st.st_size != (off_t)(sizeof(msg) - 1)) {
		printf("MLIBC_A20: FAIL stat\n");
		return 6;
	}

	pthread_t th[4];
	for (int i = 0; i < 4; i++) {
		if (pthread_create(&th[i], NULL, worker, (void *)(size_t)i) != 0) {
			printf("MLIBC_A20: FAIL pthread_create %d\n", i);
			return 7;
		}
	}
	for (int i = 0; i < 4; i++)
		pthread_join(th[i], NULL);

	if (g_counter != 400) {
		printf("MLIBC_A20: FAIL mutex counter=%d\n", g_counter);
		return 8;
	}
	for (int i = 0; i < 4; i++) {
		if (!g_seen[i]) {
			printf("MLIBC_A20: FAIL thread %d missing\n", i);
			return 9;
		}
	}

	/* pipe roundtrip */
	int pfd[2];
	if (pipe(pfd) != 0) {
		printf("MLIBC_A20: FAIL pipe\n");
		return 10;
	}
	const char pmsg[] = "pipe-bytes";
	if (write(pfd[1], pmsg, sizeof(pmsg)) != (ssize_t)sizeof(pmsg)) {
		printf("MLIBC_A20: FAIL pipe write\n");
		return 11;
	}
	char pbuf[32] = {0};
	if (read(pfd[0], pbuf, sizeof(pmsg)) != (ssize_t)sizeof(pmsg) ||
	    memcmp(pbuf, pmsg, sizeof(pmsg)) != 0) {
		printf("MLIBC_A20: FAIL pipe read\n");
		return 12;
	}

	/* poll on the pipe: empty now, then readable after write */
	struct pollfd pfd_poll = { .fd = pfd[0], .events = POLLIN };
	if (poll(&pfd_poll, 1, 0) != 0) {
		printf("MLIBC_A20: FAIL poll empty revents=%x\n", pfd_poll.revents);
		return 13;
	}
	if (write(pfd[1], "x", 1) != 1 || poll(&pfd_poll, 1, 1000) != 1 ||
	    !(pfd_poll.revents & POLLIN)) {
		printf("MLIBC_A20: FAIL poll readable revents=%x\n", pfd_poll.revents);
		return 14;
	}
	char one;
	read(pfd[0], &one, 1);
	close(pfd[0]);
	close(pfd[1]);

	/* socketpair roundtrip */
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		printf("MLIBC_A20: FAIL socketpair\n");
		return 15;
	}
	if (send(sv[0], "ping", 4, 0) != 4) {
		printf("MLIBC_A20: FAIL socketpair send\n");
		return 16;
	}
	char sbuf[8] = {0};
	if (recv(sv[1], sbuf, 4, 0) != 4 || memcmp(sbuf, "ping", 4) != 0) {
		printf("MLIBC_A20: FAIL socketpair recv\n");
		return 17;
	}
	close(sv[0]);
	close(sv[1]);

	/* posix_spawn + waitpid */
	pid_t child;
	char *cargv[] = { (char *)"/bin/mlibc-child-rv", (char *)"hello-spawn", NULL };
	int sret = posix_spawn(&child, "/bin/mlibc-child-rv", NULL, NULL, cargv, NULL);
	if (sret != 0) {
		printf("MLIBC_A20: FAIL posix_spawn ret=%d\n", sret);
		return 18;
	}
	int wstatus = 0;
	pid_t wp = waitpid(child, &wstatus, 0);
	if (wp != child || !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 42) {
		printf("MLIBC_A20: FAIL waitpid wp=%d status=%x\n", (int)wp, wstatus);
		return 19;
	}

	printf("MLIBC_A20: PASS\n");
	return 0;
}
