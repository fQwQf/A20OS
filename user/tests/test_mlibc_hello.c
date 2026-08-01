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
#include <sys/select.h>
#include <time.h>
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
	struct timespec zero_ts = {0, 0};
	if (ppoll(&pfd_poll, 1, &zero_ts, NULL) != 1 ||
	    !(pfd_poll.revents & POLLIN)) {
		printf("MLIBC_A20: FAIL ppoll revents=%x errno=%d\n", pfd_poll.revents, errno);
		return 32;
	}
	char one;
	read(pfd[0], &one, 1);
	fd_set read_set;
	FD_ZERO(&read_set);
	FD_SET(pfd[0], &read_set);
	if (pselect(pfd[0] + 1, &read_set, NULL, NULL, &zero_ts, NULL) != 0 ||
	    FD_ISSET(pfd[0], &read_set)) {
		printf("MLIBC_A20: FAIL pselect empty errno=%d\n", errno);
		return 33;
	}
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
	if (fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK) != 0 ||
	    recv(sv[1], sbuf, sizeof(sbuf), 0) != -1 ||
	    (errno != EAGAIN && errno != EWOULDBLOCK)) {
		printf("MLIBC_A20: FAIL socket nonblock errno=%d\n", errno);
		return 34;
	}
	close(sv[0]);
	close(sv[1]);

	/* Native cwd is a directory capability, but POSIX callers still expect
	 * chdir/fchdir/getcwd to update the process-visible path. */
	int root_fd = open("/", O_RDONLY);
	char cwd_buf[64] = {0};
	int relative_fd = -1;
	if (root_fd < 0 || chdir("/bin") != 0 || !getcwd(cwd_buf, sizeof(cwd_buf)) ||
	    strcmp(cwd_buf, "/bin") != 0 ||
	    (relative_fd = open("sh", O_RDONLY)) < 0 || fchdir(root_fd) != 0 ||
	    !getcwd(cwd_buf, sizeof(cwd_buf)) || strcmp(cwd_buf, "/") != 0) {
		printf("MLIBC_A20: FAIL cwd errno=%d path=%s\n", errno, cwd_buf);
		if (relative_fd >= 0)
			close(relative_fd);
		return 20;
	}
	close(relative_fd);
	close(root_fd);

	/* fd-local status flags and duplication must not depend on Linux fcntl. */
	int flag_fd = open("/mlibc_test.txt", O_RDONLY);
	int duplicate_fd = -1;
	if (flag_fd < 0 || fcntl(flag_fd, F_GETFD) != 0 ||
	    fcntl(flag_fd, F_SETFD, FD_CLOEXEC) != 0 ||
	    !(fcntl(flag_fd, F_GETFD) & FD_CLOEXEC) ||
	    fcntl(flag_fd, F_SETFL, fcntl(flag_fd, F_GETFL) | O_NONBLOCK) != 0 ||
	    !(fcntl(flag_fd, F_GETFL) & O_NONBLOCK) ||
	    (duplicate_fd = fcntl(flag_fd, F_DUPFD_CLOEXEC, 10)) < 10 ||
	    !(fcntl(duplicate_fd, F_GETFD) & FD_CLOEXEC)) {
		printf("MLIBC_A20: FAIL fcntl errno=%d\n", errno);
		if (duplicate_fd >= 0)
			close(duplicate_fd);
		if (flag_fd >= 0)
			close(flag_fd);
		return 25;
	}
	int dup3_fd = dup3(flag_fd, 11, O_CLOEXEC);
	if (dup3_fd < 11 || !(fcntl(dup3_fd, F_GETFD) & FD_CLOEXEC)) {
		printf("MLIBC_A20: FAIL dup3 errno=%d\n", errno);
		if (dup3_fd >= 0)
			close(dup3_fd);
		close(flag_fd);
		return 30;
	}
	close(dup3_fd);
	close(duplicate_fd);
	close(flag_fd);

	/* dirfd-relative filesystem operations must stay within the capability. */
	int at_dir = -1, at_file = -1;
	char link_target[32] = {0};
	if (mkdirat(AT_FDCWD, "/mlibc_at_dir", 0700) != 0 ||
	    (at_dir = open("/mlibc_at_dir", O_RDONLY | O_DIRECTORY)) < 0 ||
	    (at_file = openat(at_dir, "source", O_RDWR | O_CREAT | O_TRUNC, 0600)) < 0 ||
	    write(at_file, "at-data", 7) != 7 || close(at_file) != 0 ||
	    symlinkat("source", at_dir, "link") != 0 ||
	    readlinkat(at_dir, "link", link_target, sizeof(link_target)) != 6 ||
	    strcmp(link_target, "source") != 0 ||
	    linkat(at_dir, "source", at_dir, "hard", 0) != 0 ||
	    renameat(at_dir, "hard", at_dir, "renamed") != 0 ||
	    unlinkat(at_dir, "renamed", 0) != 0 ||
	    unlinkat(at_dir, "link", 0) != 0 ||
	    unlinkat(at_dir, "source", 0) != 0 ||
	    close(at_dir) != 0 ||
	    unlinkat(AT_FDCWD, "/mlibc_at_dir", AT_REMOVEDIR) != 0) {
		printf("MLIBC_A20: FAIL dirfd errno=%d link=%s\n", errno, link_target);
		if (at_file >= 0)
			close(at_file);
		if (at_dir >= 0)
			close(at_dir);
		return 31;
	}

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

	/* posix_spawn cwd actions: path-based and fd-based variants. */
	posix_spawn_file_actions_t actions;
	char *cwd_argv[] = { (char *)"/bin/mlibc-child-rv", (char *)"cwd-spawn", NULL };
	if (posix_spawn_file_actions_init(&actions) != 0 ||
	    posix_spawn_file_actions_addchdir_np(&actions, "/bin") != 0 ||
	    posix_spawn(&child, "/bin/mlibc-child-rv", &actions, NULL, cwd_argv, NULL) != 0) {
		printf("MLIBC_A20: FAIL spawn addchdir\n");
		return 21;
	}
	posix_spawn_file_actions_destroy(&actions);
	wp = waitpid(child, &wstatus, 0);
	if (wp != child || !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 42) {
		printf("MLIBC_A20: FAIL wait addchdir status=%x\n", wstatus);
		return 22;
	}

	int cwd_fd = open("/bin", O_RDONLY);
	if (cwd_fd < 0 || posix_spawn_file_actions_init(&actions) != 0 ||
	    posix_spawn_file_actions_addfchdir_np(&actions, cwd_fd) != 0 ||
	    posix_spawn(&child, "/bin/mlibc-child-rv", &actions, NULL, cwd_argv, NULL) != 0) {
		printf("MLIBC_A20: FAIL spawn addfchdir errno=%d\n", errno);
		return 23;
	}
	posix_spawn_file_actions_destroy(&actions);
	close(cwd_fd);
	wp = waitpid(child, &wstatus, 0);
	if (wp != child || !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 42) {
		printf("MLIBC_A20: FAIL wait addfchdir status=%x\n", wstatus);
		return 24;
	}

	/* Non-stdio file actions use the Native spawn handle transfer range. */
	int action_file = open("/spawn_action.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (action_file < 0 || write(action_file, "hello\n", 6) != 6) {
		printf("MLIBC_A20: FAIL spawn action setup errno=%d\n", errno);
		if (action_file >= 0)
			close(action_file);
		return 26;
	}
	close(action_file);
	if (posix_spawn_file_actions_init(&actions) != 0 ||
	    posix_spawn_file_actions_addopen(&actions, 3, "/spawn_action.txt", O_RDONLY, 0) != 0 ||
	    posix_spawn_file_actions_adddup2(&actions, 3, 4) != 0 ||
	    posix_spawn_file_actions_addclose(&actions, 3) != 0) {
		printf("MLIBC_A20: FAIL spawn fd action init errno=%d\n", errno);
		return 27;
	}
	char *fd_argv[] = { (char *)"/bin/mlibc-child-rv", (char *)"fd-actions", NULL };
	if (posix_spawn(&child, "/bin/mlibc-child-rv", &actions, NULL, fd_argv, NULL) != 0) {
		printf("MLIBC_A20: FAIL spawn fd action errno=%d\n", errno);
		posix_spawn_file_actions_destroy(&actions);
		return 28;
	}
	posix_spawn_file_actions_destroy(&actions);
	wp = waitpid(child, &wstatus, 0);
	if (wp != child || !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 42) {
		printf("MLIBC_A20: FAIL wait fd action status=%x\n", wstatus);
		return 29;
	}
	unlink("/spawn_action.txt");

	/* Cross-process kill: spawn a child that registers a SIGUSR1 handler and
	 * sleeps at a checkpoint; kill(pid, SIGUSR1) must wake it and the
	 * handler runs at the checkpoint. */
	{
		char *sig_argv[] = { (char *)"/bin/mlibc-child-rv", (char *)"signal-spawn", NULL };
		if (posix_spawn(&child, "/bin/mlibc-child-rv", NULL, NULL, sig_argv, NULL) != 0) {
			printf("MLIBC_A20: FAIL spawn signal errno=%d\n", errno);
			return 31;
		}
		usleep(50000);   /* let the child register its handler */
		if (kill(child, SIGUSR1) != 0) {
			printf("MLIBC_A20: FAIL kill SIGUSR1 errno=%d\n", errno);
			return 32;
		}
		wp = waitpid(child, &wstatus, 0);
		if (wp != child || !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 77) {
			printf("MLIBC_A20: FAIL wait signal status=%x\n", wstatus);
			return 33;
		}
	}

	printf("MLIBC_A20: PASS\n");
	return 0;
}
