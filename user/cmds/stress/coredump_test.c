/* Coredump test: exercise the kernel ELF core-dump path end to end.
 *
 * Scenario:
 *   1. Verify /proc/sys/kernel/core_pattern round-trips through procfs.
 *   2. Set RLIMIT_CORE to infinity, fork a child that dies on SIGSEGV, and
 *      confirm waitpid reports WIFSIGNALED|WCOREDUMP.
 *   3. Open the generated core file (pattern "/tmp/cdump.%p") and parse it:
 *      ELF64 magic, ET_CORE, at least one PT_LOAD, and a PT_NOTE segment
 *      carrying NT_PRSTATUS/NT_FPREGSET/NT_PRPSINFO notes named "CORE".
 *   4. Negative gate: a child with RLIMIT_CORE=0 must leave no core file.
 */
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CORE_PATTERN_PATH "/proc/sys/kernel/core_pattern"
#define CORE_TEMPLATE     "/tmp/cdump.%p"
#define CORE_TEMPLATE2    "/tmp/cdump2.%p"

static int fail(const char *what)
{
    printf("COREDUMP_TEST: FAIL at %s (errno=%d)\n", what, errno);
    return 1;
}

static int write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    size_t len = strlen(text);
    int ok = write(fd, text, len) == (ssize_t)len;
    close(fd);
    return ok ? 0 : -1;
}

static int read_text(const char *path, char *buf, size_t bufsz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return 0;
}

static void crash_child(void)
{
    /* Deliberate wild write; must die from the signal, not return. */
    volatile unsigned long *p = (volatile unsigned long *)0;
    *p = 0xdeadbeef;
    _exit(42);
}

/* Expand the %p placeholder of a core_pattern into `out`. */
static void expand_pattern_pid(const char *pattern, pid_t pid, char *out,
                               size_t outsz)
{
    size_t o = 0;
    for (const char *p = pattern; *p && o + 1 < outsz; p++) {
        if (p[0] == '%' && p[1] == 'p') {
            o += snprintf(out + o, outsz - o, "%d", (int)pid);
            p++;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

/* Parse the core file at `path`: ELF64/ET_CORE, >= 1 PT_LOAD, PT_NOTE with
 * NT_PRSTATUS + NT_PRPSINFO + NT_FPREGSET notes. */
static int check_core_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return fail("core file open");
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return fail("core file fstat");
    }
    if (st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
        close(fd);
        printf("COREDUMP_TEST: FAIL core too small (%lld)\n",
               (long long)st.st_size);
        return 1;
    }
    uint8_t *buf = malloc((size_t)st.st_size);
    if (!buf) {
        close(fd);
        return fail("malloc");
    }
    size_t done = 0;
    while (done < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + done, (size_t)st.st_size - done);
        if (n <= 0) {
            free(buf);
            close(fd);
            return fail("core file read");
        }
        done += (size_t)n;
    }
    close(fd);

    int rc = 1;
    Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
        printf("COREDUMP_TEST: FAIL bad ELF magic\n");
        goto out;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        printf("COREDUMP_TEST: FAIL not ELF64-LSB\n");
        goto out;
    }
    if (eh->e_type != ET_CORE) {
        printf("COREDUMP_TEST: FAIL e_type=%u (want ET_CORE)\n", eh->e_type);
        goto out;
    }
    if (eh->e_phoff != sizeof(Elf64_Ehdr) || eh->e_phentsize != sizeof(Elf64_Phdr) ||
        eh->e_phnum < 2) {
        printf("COREDUMP_TEST: FAIL phdr layout (off=%llu entsz=%u num=%u)\n",
               (unsigned long long)eh->e_phoff, eh->e_phentsize, eh->e_phnum);
        goto out;
    }

    int nload = 0;
    const Elf64_Phdr *note_ph = NULL;
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph =
            (const Elf64_Phdr *)(buf + eh->e_phoff + (size_t)i * eh->e_phentsize);
        if (ph->p_type == PT_LOAD) {
            nload++;
            if (ph->p_filesz > ph->p_memsz) {
                printf("COREDUMP_TEST: FAIL PT_LOAD filesz > memsz\n");
                goto out;
            }
        } else if (ph->p_type == PT_NOTE) {
            note_ph = ph;
        }
    }
    if (nload == 0) {
        printf("COREDUMP_TEST: FAIL no PT_LOAD segments\n");
        goto out;
    }
    if (!note_ph || note_ph->p_offset + note_ph->p_filesz > (uint64_t)st.st_size) {
        printf("COREDUMP_TEST: FAIL missing/invalid PT_NOTE\n");
        goto out;
    }

    int seen_prstatus = 0, seen_prpsinfo = 0, seen_fpregset = 0;
    const uint8_t *p = buf + note_ph->p_offset;
    const uint8_t *end = p + note_ph->p_filesz;
    while (p + sizeof(Elf64_Nhdr) <= end) {
        const Elf64_Nhdr *nh = (const Elf64_Nhdr *)p;
        p += sizeof(Elf64_Nhdr);
        if (p + nh->n_namesz > end)
            break;
        const char *name = (const char *)p;
        p += (nh->n_namesz + 3) & ~3u;
        if (p + nh->n_descsz > end)
            break;
        int named_core = nh->n_namesz >= 5 && memcmp(name, "CORE", 4) == 0;
        if (named_core && nh->n_type == NT_PRSTATUS)
            seen_prstatus = 1;
        else if (named_core && nh->n_type == NT_PRPSINFO)
            seen_prpsinfo = 1;
        else if (named_core && nh->n_type == NT_FPREGSET)
            seen_fpregset = 1;
        p += (nh->n_descsz + 3) & ~3u;
    }
    if (!seen_prstatus || !seen_prpsinfo || !seen_fpregset) {
        printf("COREDUMP_TEST: FAIL notes prstatus=%d prpsinfo=%d fpregset=%d\n",
               seen_prstatus, seen_prpsinfo, seen_fpregset);
        goto out;
    }
    printf("COREDUMP_TEST: core %s ok (%lld bytes, %d PT_LOAD)\n",
           path, (long long)st.st_size, nload);
    rc = 0;
out:
    free(buf);
    return rc;
}

int main(void)
{
    char buf[128];
    /* CORE_PATTERN env overrides the dump location (used by host-side
     * verification that extracts the file from a persistent filesystem). */
    const char *pattern = getenv("CORE_PATTERN");
    if (!pattern || !pattern[0])
        pattern = CORE_TEMPLATE;

    /* 1. core_pattern default + write/read round-trip through procfs. */
    if (read_text(CORE_PATTERN_PATH, buf, sizeof(buf)) < 0)
        return fail("read core_pattern");
    if (strcmp(buf, "core\n") != 0) {
        printf("COREDUMP_TEST: FAIL default core_pattern is '%s'\n", buf);
        return 1;
    }
    if (write_text(CORE_PATTERN_PATH, pattern) < 0)
        return fail("write core_pattern");
    if (read_text(CORE_PATTERN_PATH, buf, sizeof(buf)) < 0)
        return fail("re-read core_pattern");
    char expect[160];
    snprintf(expect, sizeof(expect), "%s\n", pattern);
    if (strcmp(buf, expect) != 0) {
        printf("COREDUMP_TEST: FAIL core_pattern round-trip got '%s'\n", buf);
        return 1;
    }

    /* 2. Crash a child with unlimited RLIMIT_CORE. */
    struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };
    if (setrlimit(RLIMIT_CORE, &rl) < 0)
        return fail("setrlimit RLIMIT_CORE");
    struct rlimit got;
    if (getrlimit(RLIMIT_CORE, &got) < 0 || got.rlim_cur != RLIM_INFINITY)
        return fail("getrlimit RLIMIT_CORE");

    pid_t pid = fork();
    if (pid < 0)
        return fail("fork");
    if (pid == 0)
        crash_child();

    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return fail("waitpid");
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV ||
        !WCOREDUMP(status)) {
        printf("COREDUMP_TEST: FAIL child status=0x%x (sig=%d core=%d)\n",
               status, WIFSIGNALED(status) ? WTERMSIG(status) : -1,
               WCOREDUMP(status) ? 1 : 0);
        return 1;
    }

    /* 3. Validate the generated ELF core. */
    char core_path[160];
    expand_pattern_pid(pattern, pid, core_path, sizeof(core_path));
    /* The file must exist once the child was reaped; allow the kernel a
     * moment of write-back slack by retrying the lookup briefly. */
    int exists = 0;
    for (int i = 0; i < 50; i++) {
        if (access(core_path, F_OK) == 0) {
            exists = 1;
            break;
        }
        usleep(10000);
    }
    if (!exists) {
        printf("COREDUMP_TEST: FAIL no core file at %s\n", core_path);
        return 1;
    }
    if (check_core_file(core_path) != 0)
        return 1;
    if (!getenv("KEEP_CORE"))
        unlink(core_path);

    /* 4. RLIMIT_CORE=0 suppresses the dump. */
    if (write_text(CORE_PATTERN_PATH, CORE_TEMPLATE2) < 0)
        return fail("write core_pattern #2");
    pid_t pid2 = fork();
    if (pid2 < 0)
        return fail("fork #2");
    if (pid2 == 0) {
        struct rlimit zero = { 0, 0 };
        setrlimit(RLIMIT_CORE, &zero);
        crash_child();
    }
    if (waitpid(pid2, &status, 0) != pid2)
        return fail("waitpid #2");
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) {
        printf("COREDUMP_TEST: FAIL child2 status=0x%x\n", status);
        return 1;
    }
    char core_path2[128];
    snprintf(core_path2, sizeof(core_path2), "/tmp/cdump2.%d", (int)pid2);
    usleep(50000);
    if (access(core_path2, F_OK) == 0) {
        printf("COREDUMP_TEST: FAIL core file exists despite RLIMIT_CORE=0\n");
        return 1;
    }

    /* Restore the default pattern for later boots/tests. */
    write_text(CORE_PATTERN_PATH, "core");

    printf("COREDUMP_TEST: PASS\n");
    return 0;
}
