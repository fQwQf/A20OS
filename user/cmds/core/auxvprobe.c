/* Audit both auxv surfaces exposed by the Linux ABI.  The initial stack and
 * /proc/PID/auxv must expose the same native-word {type,value} pairs, including
 * the terminating {AT_NULL,0}.  This precondition audit does not claim FEX is
 * supported. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>

#define AT_NULL       0
#define AT_PHDR       3
#define AT_PHENT      4
#define AT_PHNUM      5
#define AT_PAGESZ     6
#define AT_ENTRY      9
#define AT_EXECFN     31

extern char **environ;

struct aux_pair {
	unsigned long type;
	unsigned long value;
};

static int count_tag(const struct aux_pair *v, size_t n, unsigned long tag,
		     unsigned long *value)
{
	int count = 0;
	for (size_t i = 0; i < n; i++) {
		if (v[i].type == tag) {
			count++;
			if (value)
				*value = v[i].value;
		}
	}
	return count;
}

static ssize_t read_proc_auxv(char *buf, size_t cap)
{
	int fd = open("/proc/self/auxv", O_RDONLY);
	if (fd < 0)
		return -1;
	size_t used = 0;
	while (used < cap) {
		/* Deliberately use short reads; the proc file must preserve the
		 * byte stream and file offset across arbitrary read boundaries. */
		size_t want = cap - used;
		if (want > 7)
			want = 7;
		ssize_t n = read(fd, buf + used, want);
		if (n < 0) {
			close(fd);
			return -1;
		}
		if (n == 0)
			break;
		used += (size_t)n;
	}
	close(fd);
	return (ssize_t)used;
}

static int audit_proc_auxv(const struct aux_pair *stack, size_t stack_n)
{
	char buf[8192];
	ssize_t nr = read_proc_auxv(buf, sizeof(buf));
	if (nr < 0) {
		printf("AUXV_PROC: open/read failed errno=%d\n", errno);
		return 1;
	}
	size_t n = (size_t)nr;
	if (n % sizeof(struct aux_pair) || n / sizeof(struct aux_pair) != stack_n) {
		printf("AUXV_PROC: malformed native-word vector bytes=%zu\n", n);
		return 1;
	}
	for (size_t i = 0; i < stack_n; i++) {
		struct aux_pair pair;
		memcpy(&pair, buf + i * sizeof(pair), sizeof(pair));
		if (pair.type != stack[i].type || pair.value != stack[i].value) {
			printf("AUXV_PROC: pair[%zu] differs from exec vector\n", i);
			return 1;
		}
	}
	struct aux_pair proc[64];
	if (stack_n > sizeof(proc) / sizeof(proc[0]))
		return 1;
	memcpy(proc, buf, n);
	unsigned long pagesz = 0;
	int valid = proc[stack_n - 1].type == AT_NULL &&
		    proc[stack_n - 1].value == 0 &&
		    count_tag(proc, stack_n, AT_NULL, NULL) == 1 &&
		    count_tag(proc, stack_n, AT_PHDR, NULL) == 1 &&
		    count_tag(proc, stack_n, AT_PHENT, NULL) == 1 &&
		    count_tag(proc, stack_n, AT_PHNUM, NULL) == 1 &&
		    count_tag(proc, stack_n, AT_ENTRY, NULL) == 1 &&
		    count_tag(proc, stack_n, AT_PAGESZ, &pagesz) == 1 &&
		    pagesz == 4096;
	printf("AUXV_PROC: binary %zu pairs, native-word, stack-match=%s, PHDR/PAGESZ/AT_NULL=%s\n",
	       stack_n, valid ? "YES" : "NO", valid ? "valid" : "INVALID");
	if (!valid)
		return 1;
	printf("AUXV: PROC_LINUX_COMPAT=YES; FEX_SUPPORT=NOT_ESTABLISHED\n");
	return 0;
}

int main(void)
{
	char **p = environ;
	while (*p)
		p++;
	const struct aux_pair *v = (const struct aux_pair *)(p + 1);
	size_t n = 0;
	while (n < 64 && v[n].type != AT_NULL)
		n++;
	if (n == 64) {
		printf("AUXV_STACK: missing AT_NULL\n");
		return 1;
	}
	n++; /* include the one AT_NULL pair */
	unsigned long pagesz = 0;
	int core_ok = count_tag(v, n, AT_PHDR, NULL) == 1 &&
		      count_tag(v, n, AT_PHENT, NULL) == 1 &&
		      count_tag(v, n, AT_PHNUM, NULL) == 1 &&
		      count_tag(v, n, AT_ENTRY, NULL) == 1 &&
		      count_tag(v, n, AT_PAGESZ, &pagesz) == 1 &&
		      count_tag(v, n, AT_NULL, NULL) == 1 && pagesz == 4096;
	int execfn = count_tag(v, n, AT_EXECFN, NULL);
	printf("AUXV_STACK: %zu pairs, 1 AT_NULL, required=%s, AT_EXECFN=%s\n",
	       n, core_ok ? "present" : "MISSING_OR_DUPLICATE",
	       execfn == 1 ? "present" : "absent");
	if (!core_ok || execfn > 1)
		return 1;
	if (audit_proc_auxv(v, n))
		return 1;
	printf("AUXV: AUDIT DONE\n");
	return 0;
}
