/* test_mlibc_pipeexec.c — isolate pipe data flow across fork+dup2+execve.
 *
 * Stage A: fork, child writes via dup2'd fd WITHOUT exec.
 * Stage B: full pipeline: child dup2 + execve /bin/mlibc-seq.
 * Every step prints a marker (to stderr where stdout is the pipe).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

#ifdef __riscv
static long a20_raw6(long nr, long a0, long a1, long a2, long a3,
                     long a4, long a5) {
	register long t0 __asm__("t0") = nr;
	register long a0r __asm__("a0") = a0;
	register long a1r __asm__("a1") = a1;
	register long a2r __asm__("a2") = a2;
	register long a3r __asm__("a3") = a3;
	register long a4r __asm__("a4") = a4;
	register long a5r __asm__("a5") = a5;
	__asm__ volatile("ecall"
			 : "+r"(a0r)
			 : "r"(t0), "r"(a0r), "r"(a1r), "r"(a2r),
			   "r"(a3r), "r"(a4r), "r"(a5r)
			 : "memory");
	return a0r;
}
static long a20_query(long fd, void *info) {
	return a20_raw6(0x0102, fd, (long)info, 0, 0, 0, 0);
}
#endif

/* Child mode: introspect the inherited stdio via raw syscalls so no libc
 * fd-table logic can mask what the kernel actually gave us. */
static int child_probe(void) {
#ifdef __riscv
	/* A20_SYS_handle_query = 0x0102 */
	struct {
		unsigned size, version, object_type, state;
		unsigned long rights, object_id_hint, flags;
	} info;
	for (int fd = 0; fd <= 2; fd++) {
		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		info.version = 1;
		long r = a20_query(fd, &info);
		dprintf(2, "PE-C/CHILD: query fd%d ret=%ld type=%u rights=%lx\n",
			fd, r, r >= 0 ? info.object_type : 0,
			r >= 0 ? info.rights : 0);
	}
	const char *msg = "from-exec";
	ssize_t w = write(1, msg, 9);
	dprintf(2, "PE-C/CHILD: write w=%zd errno=%d\n", w, errno);
	for (int i = 0; i < 3; i++) {
		printf("tick%d\n", i);
		fflush(stdout);
		usleep(20000);
	}
	return w == 9 ? 0 : 1;
#else
	return 1;
#endif
}

static int drain(int fd, char *buf, size_t cap) {
	size_t got = 0;
	for (;;) {
		if (got >= cap - 1)
			break;
		ssize_t n = read(fd, buf + got, cap - 1 - got);
		if (n < 0) {
			dprintf(2, "PE-TEST: read failed at %zu errno=%d\n",
				got, errno);
			return -1;
		}
		if (n == 0)
			break;
		got += (size_t)n;
	}
	buf[got] = '\0';
	return (int)got;
}

int main(int argc, char **argv) {
	if (argc > 1 && strcmp(argv[1], "childprobe") == 0)
		return child_probe();

	int fds[2];
	if (pipe(fds) != 0) {
		dprintf(2, "PE-TEST: pipe failed errno=%d\n", errno);
		return 1;
	}

	/* ---- Stage A: fork + dup2 + write, NO exec ---- */
	pid_t pid = fork();
	if (pid == 0) {
		close(fds[0]);
		if (dup2(fds[1], 1) != 1)
			dprintf(2, "PE-A/CHILD: dup2 failed errno=%d\n", errno);
		ssize_t w = write(1, "alpha", 5);
		dprintf(2, "PE-A/CHILD: write w=%zd errno=%d\n", w, errno);
		_exit(w == 5 ? 0 : 9);
	}
	close(fds[1]);
	char buf[64] = {0};
	int n = drain(fds[0], buf, sizeof(buf));
	int st = 0;
	waitpid(pid, &st, 0);
	dprintf(2, "PE-A: bytes=%d data=[%s] status=0x%x\n", n, buf, st);

	/* ---- Stage A2: fork + dup2 + MULTIPLE writes over time, no exec ---- */
	if (pipe(fds) != 0) {
		dprintf(2, "PE-TEST: pipeA2 failed errno=%d\n", errno);
		return 1;
	}
	pid = fork();
	if (pid == 0) {
		close(fds[0]);
		if (dup2(fds[1], 1) != 1)
			dprintf(2, "PE-A2/CHILD: dup2 failed errno=%d\n", errno);
		for (int i = 0; i < 3; i++) {
			printf("tick%d\n", i);
			fflush(stdout);
			usleep(20000);
		}
		_exit(0);
	}
	close(fds[1]);
	memset(buf, 0, sizeof(buf));
	n = drain(fds[0], buf, sizeof(buf));
	st = 0;
	waitpid(pid, &st, 0);
	dprintf(2, "PE-A2: bytes=%d data=[%s] status=0x%x\n", n, buf, st);

	/* ---- Stage B: fork + dup2 + execve ---- */
	if (pipe(fds) != 0) {
		dprintf(2, "PE-TEST: pipe2 failed errno=%d\n", errno);
		return 1;
	}
	pid = fork();
	if (pid == 0) {
		close(fds[0]);
		if (dup2(fds[1], 1) != 1)
			dprintf(2, "PE-B/CHILD: dup2 failed errno=%d\n", errno);
		close(fds[1]);
		if (getenv("PE_USE_SEQ"))
			execl("/bin/mlibc-seq", "mlibc-seq", "1", "5", (char *)NULL);
		else
			execl("/bin/mlibc-pipeexec-rv", "mlibc-pipeexec-rv",
			      "childprobe", (char *)NULL);
		dprintf(2, "PE-B/CHILD: exec failed errno=%d\n", errno);
		_exit(127);
	}
	close(fds[1]);
	memset(buf, 0, sizeof(buf));
	n = drain(fds[0], buf, sizeof(buf));
	st = 0;
	waitpid(pid, &st, 0);
	dprintf(2, "PE-B: bytes=%d data=[%s] status=0x%x\n", n, buf, st);

	printf("%s\n", n > 0 ? "PIPEEXEC: PASS" : "PIPEEXEC: FAIL");
	return n > 0 ? 0 : 1;
}
