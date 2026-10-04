/*
 * A20OS — execve translation harness (positive + negative)
 *
 * Drives execve() from a Linux-ABI process and checks what came back.
 * It never names a translator: the point is that `xlate_exec ok <path>`
 * runs <path> the same way any program would, and the kernel either
 * loads it natively or transparently hands it to a user-space
 * translator.
 *
 *   xlate_exec ok           <path> [args...]  child must exit XLATE_PROBE_EXIT
 *   xlate_exec enoexec      <path>            execve must fail with ENOEXEC
 *   xlate_exec unconfigured <path>            execve must fail with ENOEXEC
 *   xlate_exec foreign      <path>            execve(<path>) must fail ENOEXEC
 *   xlate_exec script       <path>            child must exit 0 as a script
 *   xlate_exec toggle       <path> [args...]  drive /proc/a20/xlator through
 *                                            on -> off -> on and assert the
 *                                            same binary translates only
 *                                            while the switch is on
 *   xlate_exec stage        <machine> <path>  write a valid ELF64 header for
 *                                            e_machine=<machine> to <path>
 *   xlate_exec run          <path> [args...]  execve(<path>) and report the
 *                                            exit code; only a failed execve
 *                                            is a failure
 *
 * The negative modes are the regression guard for the translator hook, and
 * each one rules out a different way of getting it wrong:
 *
 *   enoexec      -- a file that is not a runnable ELF at all must still
 *                   produce ENOEXEC, i.e. the hook does not read past the
 *                   header checks looking for a chance to translate.
 *   unconfigured -- a *valid* ELF for a registered guest, with no
 *                   translator configured for it, must still produce
 *                   ENOEXEC.  This is the one that distinguishes "the
 *                   administrator configured a translator" from "the kernel
 *                   was built knowing about this architecture".
 *   foreign      -- unlike the two above, this one execs the path it is
 *                   actually given instead of a fixture.  It is what says
 *                   something about a *real* binary in the image: `enoexec`
 *                   against a missing file would also report ENOEXEC, but
 *                   it would report ENOEXEC from execve's own lookup, and
 *                   a smoke that passed on a missing file would be
 *                   asserting nothing.
 *   script       -- a #! script must still reach its interpreter rather
 *                   than be handed to a translator.
 *
 * stage and run are the pair that make the *invocation* observable: stage
 * writes a well-formed foreign ELF header for whichever e_machine is asked
 * for, and run execs it.  Pointing the channel at user/cmds/core/xlate_shim.c
 * instead of a real translator then turns "what argv did the kernel build?"
 * into a question the log answers directly, which is what
 * smoke-exec-xlator-shim relies on.  Neither mode needs a downloaded binary
 * or a cross compiler, so that smoke is hermetic.
 *
 * All fixtures are written by this program under /tmp so the image needs no
 * extra files.
 *
 * Exit 0 = the mode's assertion held; 1 = it did not.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define XLATE_PROBE_EXIT 42

static int write_file(const char *path, const char *data, size_t len)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0755);
    if (fd < 0) {
        fprintf(stderr, "xlate_exec: open %s: %s\n", path, strerror(errno));
        return -1;
    }
    ssize_t w = write(fd, data, len);
    close(fd);
    if (w != (ssize_t)len) {
        fprintf(stderr, "xlate_exec: write %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

/* A file that starts with a real ELF magic but has a nonsense machine and
 * a truncated program header table.  The translator hook must reject it
 * exactly as elf_load() does, not mistake the magic for a guest. */
static const char CORRUPT_ELF[] =
    "\x7f" "ELF" "\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\xff\xff\x00\x00\x00\x00\x00\x00"   /* e_type=ET_NONE */
    "\x99\x99"                           /* e_machine = 0x9999, unknown */
    "\x01\x00\x00\x00";

/* A header that is *valid* in every field the foreign-arch check looks at --
 * magic, ELFCLASS64, little-endian, ET_EXEC, a full-size e_phentsize and a
 * non-zero e_phnum -- and then names e_machine 183 (aarch64).
 *
 * 183 is deliberately a registered guest: it is a row in
 * kernel/proc/xlator_guests.def.  So this file gets past everything a
 * corrupt header would be stopped by, and the only reason it can still come
 * back ENOEXEC is that no translator path was configured for aarch64 on the
 * command line.  That is the assertion this fixture exists for: the gate is
 * "did the administrator configure a translator", not "is this machine in a
 * list the kernel was compiled with".
 *
 * On an aarch64 host this weakens to a generic load failure, because 183 is
 * native there and the header has no program headers behind it; the smoke
 * that uses it runs on riscv64.
 */
static const char UNCONFIGURED_ELF[] =
    "\x7f" "ELF" "\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x02\x00"                           /* e_type = ET_EXEC */
    "\xb7\x00"                           /* e_machine = 183 (aarch64) */
    "\x01\x00\x00\x00"                   /* e_version = EV_CURRENT */
    "\x00\x00\x00\x00\x00\x00\x00\x00"   /* e_entry */
    "\x40\x00\x00\x00\x00\x00\x00\x00"   /* e_phoff = 64 */
    "\x00\x00\x00\x00\x00\x00\x00\x00"   /* e_shoff */
    "\x00\x00\x00\x00"                   /* e_flags */
    "\x40\x00"                           /* e_ehsize = 64 */
    "\x38\x00"                           /* e_phentsize = 56 */
    "\x01\x00"                           /* e_phnum = 1 */
    "\x00\x00\x00\x00\x00\x00";

static const char SHEBANG_SCRIPT[] = "#!/bin/echo\nSHEBANG_RAN\n";

/* A header valid in every field the foreign-arch check inspects, with the
 * e_machine patched in by stage().  Built by copying UNCONFIGURED_ELF and
 * overwriting the two e_machine bytes, so there is exactly one fixture shape
 * to keep in step with kernel/mm/elf.c's checks. */
#define ELF_E_MACHINE_OFF 18
#define ELF_E_MACHINE_LEN 2

static int stage_fixture(unsigned long machine, const char *path)
{
    char hdr[sizeof(UNCONFIGURED_ELF) - 1];

    if (machine > 0xFFFF) {
        fprintf(stderr, "xlate_exec: stage: e_machine %lu does not fit in "
                "two bytes\n", machine);
        return -1;
    }
    memcpy(hdr, UNCONFIGURED_ELF, sizeof(hdr));
    hdr[ELF_E_MACHINE_OFF]     = (char)(machine & 0xFF);
    hdr[ELF_E_MACHINE_OFF + 1] = (char)((machine >> 8) & 0xFF);

    return write_file(path, hdr, sizeof(hdr));
}

static int mode_stage(char *const argv[])
{
    /* argv[0] is <e_machine>, argv[1] the destination path: the caller passes
     * &argv[2] so the first two are the mode's own operands. */
    if (stage_fixture(strtoul(argv[0], NULL, 0), argv[1]) < 0)
        return 1;
    printf("XLATE_EXEC: stage machine=%s path=%s\n", argv[0], argv[1]);
    return 0;
}

/* Fork, exec @path with @argv, wait, and return the child's exit code in
 * *status_out.  A child that could not be exec'd is reported as
 * *exec_errno_out != 0.
 *
 * The failure channel is a plain pipe rather than an FD_CLOEXEC close:
 * a CLOEXEC close fires when the *translator* execs, which is long before
 * the guest finishes, so the parent would enter waitpid while the child is
 * still translating.  That is harmless for the exit code but it couples
 * the harness to exactly the timing this smoke is trying to measure. */
static int run_child(const char *path, char *const argv[],
                     int *status_out, int *exec_errno_out)
{
    *status_out = -1;
    *exec_errno_out = 0;

    /* A dedicated pipe so the child can report an execve failure back:
     * _exit(127) after a failed exec is indistinguishable from a guest
     * that legitimately returned 127. */
    int pfd[2];
    if (pipe(pfd) < 0) {
        perror("xlate_exec: pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("xlate_exec: fork");
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pfd[0]);
        /* execv() @path, not argv[0]: run --argv0= deliberately makes them
         * differ, and exec'ing argv[0] would try to run the decoy. */
        execv(path, argv);
        int e = errno;
        ssize_t ignored = write(pfd[1], &e, sizeof(e));
        (void)ignored;
        _exit(127);
    }

    close(pfd[1]);
    int child_errno = 0;
    ssize_t r = read(pfd[0], &child_errno, sizeof(child_errno));
    close(pfd[0]);

    if (r == (ssize_t)sizeof(child_errno))
        *exec_errno_out = child_errno;

    /* Do not fall through with a zeroed status: a failed wait would be
     * indistinguishable from a child that legitimately exited 0, which is
     * exactly the ambiguity this harness exists to rule out. */
    int st = 0;
    pid_t w;
    do {
        w = waitpid(pid, &st, 0);
    } while (w < 0 && errno == EINTR);
    if (w != pid) {
        fprintf(stderr, "xlate_exec: waitpid(%d): %s\n", pid, strerror(errno));
        return -1;
    }

    if (WIFEXITED(st))
        *status_out = WEXITSTATUS(st);
    else if (WIFSIGNALED(st))
        *status_out = 128 + WTERMSIG(st);
    else
        *status_out = -1;
    return 0;
}

/* execve and report, without judging the exit code.
 *
 * "run" is for observing whatever the kernel forwarded *to*: the program it
 * lands on may legitimately exit non-zero (a translator that forwards its
 * guest's status does exactly that), so treating any non-zero code as a
 * failure would make this mode useless for the thing it exists for.  Only a
 * failed execve is a failure -- that means the kernel did not forward at all,
 * which is what the caller needs to know.
 *
 * --argv0=<s> makes the child see @s as its argv[0] while still execing
 * <path>.  That is the only way to observe the @A substitution, which is the
 * one part of the template whose value comes from the caller rather than from
 * the configuration: with execv() alone, argv[0] and the path are always the
 * same string and a broken @A would be invisible. */
static int mode_run(char **argv, int argc)
{
    char **child = argv;
    const char *path;

    if (strncmp(argv[0], "--argv0=", 8) == 0) {
        /* argv[0] is the decoy the child should *see*; the program to exec is
         * argv[1] and the child's real arguments are argv[2..].
         *
         * The path deliberately does NOT go into the child's argv.  execv
         * takes it separately, and putting it there as well would make it
         * show up twice in anything that expands the guest path -- a @P
         * template plus a @* splice would both supply it, and the duplicate
         * would read as a kernel bug rather than as harness bookkeeping. */
        static char *buf[64];
        if (argc < 2) {
            fprintf(stderr, "xlate_exec: run --argv0= needs a path\n");
            return 2;
        }
        if (argc > (int)(sizeof(buf) / sizeof(buf[0])) - 1) {
            fprintf(stderr, "xlate_exec: run: too many arguments\n");
            return 2;
        }
        buf[0] = argv[0] + 8;
        for (int i = 2; i < argc; i++)
            buf[i - 1] = argv[i];
        buf[argc - 1] = NULL;
        child = buf;
        path = argv[1];
    } else {
        path = argv[0];
    }

    /* Same precheck `foreign` has.  A missing file would also make execve
     * fail, and "run" reports that as a failure of the channel -- so the
     * fixture has to be known-present before the result means anything. */
    if (access(path, F_OK) != 0) {
        printf("XLATE_EXEC: run FAIL: %s does not exist, so an execve failure "
               "would prove nothing\n", path);
        return 1;
    }

    int st, e;
    if (run_child(path, child, &st, &e) < 0)
        return 1;
    if (e != 0) {
        printf("XLATE_EXEC: run FAIL: execve(%s) failed: %s\n",
               path, strerror(e));
        return 1;
    }
    printf("XLATE_EXEC: run exit=%d\n", st);
    return 0;
}

static int mode_ok(char *const argv[])
{
    int st, e;
    if (run_child(argv[0], argv, &st, &e) < 0)
        return 1;
    if (e != 0) {
        printf("XLATE_EXEC: ok FAIL: execve(%s) failed: %s\n",
               argv[0], strerror(e));
        return 1;
    }
    if (st != XLATE_PROBE_EXIT) {
        printf("XLATE_EXEC: ok FAIL: expected exit %d, got %d\n",
               XLATE_PROBE_EXIT, st);
        return 1;
    }
    printf("XLATE_EXEC: ok PASS: exit=%d\n", st);
    return 0;
}

/* `label` is the subcommand's own name: enoexec and unconfigured assert the
 * same errno for very different reasons, and a log that says only "enoexec
 * PASS" for both cannot tell you which one ran. */
static int mode_enoexec(char *const argv[], const char *label)
{
    int st, e;
    if (run_child(argv[0], argv, &st, &e) < 0)
        return 1;
    if (e != ENOEXEC) {
        printf("XLATE_EXEC: %s FAIL: expected ENOEXEC, got %s (exit %d)\n",
               label, e ? strerror(e) : "no error", st);
        return 1;
    }
    printf("XLATE_EXEC: %s PASS: ENOEXEC\n", label);
    return 0;
}

static int mode_script(char *const argv[])
{
    int st, e;
    if (run_child(argv[0], argv, &st, &e) < 0)
        return 1;
    if (e != 0) {
        printf("XLATE_EXEC: script FAIL: execve(%s) failed: %s\n",
               argv[0], strerror(e));
        return 1;
    }
    if (st != 0) {
        printf("XLATE_EXEC: script FAIL: expected exit 0, got %d\n", st);
        return 1;
    }
    printf("XLATE_EXEC: script PASS: exit=0\n");
    return 0;
}

#define XLATOR_PROC "/proc/a20/xlator"

/* Read /proc/a20/xlator and return the value of "enabled: N", or -1.
 * The other lines are the forwarding counter and the per-guest table; only
 * the switch matters here, and parsing one field beats string-matching the
 * whole render -- a counter going from 0 to 1 must not read as the switch
 * having flipped. */
static int read_enabled(int *out)
{
    char buf[512];
    int fd = open(XLATOR_PROC, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    const char *p = strstr(buf, "enabled:");
    if (!p)
        return -1;
    p += strlen("enabled:");
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p < '0' || *p > '9')
        return -1;
    *out = *p - '0';
    return 0;
}

/* Write @text to the switch node with a plain O_WRONLY open -- no O_TRUNC,
 * no shell redirect.  A shell's `>` adds O_CREAT|O_TRUNC, and whether the
 * procfs vnode tolerates those is a property of this kernel's open path
 * rather than of the feature; using explicit flags keeps the test asserting
 * the switch and not the open() flavour. */
static int write_switch(const char *text)
{
    int fd = open(XLATOR_PROC, O_WRONLY);
    if (fd < 0)
        return errno;
    size_t len = strlen(text);
    ssize_t w = write(fd, text, len);
    int e = errno;
    close(fd);
    return (w == (ssize_t)len) ? 0 : e;
}

static int read_enabled_or_die(const char *where)
{
    int v;
    if (read_enabled(&v) < 0) {
        printf("XLATE_EXEC: toggle FAIL: cannot read %s at %s: %s\n",
               XLATOR_PROC, where, strerror(errno));
        return -1;
    }
    return v;
}

/* The runtime switch, end to end.  Runs the whole loop in one process so
 * that "enabled", "disabled" and "enabled again" are observed on the same
 * host state rather than inferred from three separate smoke commands whose
 * ordering the harness cannot guarantee.
 *
 *   1. assert the node exists and reads back enabled=1
 *   2. execve the probe  -> must succeed        (already known, but it is the
 *                                                   control: without it the
 *                                                   later ENOEXEC proves nothing)
 *   3. write 0, re-read  -> must read 0
 *   4. execve the same probe -> must be ENOEXEC
 *   5. write 1, re-read  -> must read 1
 *   6. execve again      -> must succeed
 *   7. write "2" and "on" -> must be rejected with EINVAL, and the switch
 *                             must still read 1 afterwards, so a rejected
 *                             write cannot have half-applied
 *
 * Step 7 exists because the alternative parser (atoi) accepts every one of
 * those as 0 -- which would look like it worked while quietly disabling the
 * channel.
 */
static int mode_toggle(char *const argv[])
{
    int v;

    v = read_enabled_or_die("start");
    if (v < 0)
        return 1;
    if (v != 1) {
        printf("XLATE_EXEC: toggle FAIL: %s reads enabled=%d, expected 1 "
               "(is a20.xlator=1 on the cmdline?)\n", XLATOR_PROC, v);
        return 1;
    }
    printf("XLATE_EXEC: toggle: node present, enabled=1\n");

    int st, e;
    if (run_child(argv[0], argv, &st, &e) < 0)
        return 1;
    if (e != 0 || st != XLATE_PROBE_EXIT) {
        printf("XLATE_EXEC: toggle FAIL: control execve(%s): %s exit %d\n",
               argv[0], e ? strerror(e) : "no error", st);
        return 1;
    }
    printf("XLATE_EXEC: toggle: step1 execve while enabled PASS (exit=%d)\n", st);

    if (write_switch("0") != 0) {
        printf("XLATE_EXEC: toggle FAIL: write 0: %s\n", strerror(errno));
        return 1;
    }
    v = read_enabled_or_die("after write 0");
    if (v < 0)
        return 1;
    if (v != 0) {
        printf("XLATE_EXEC: toggle FAIL: after write 0 the node reads %d\n", v);
        return 1;
    }
    printf("XLATE_EXEC: toggle: step2 wrote 0, node reads enabled=0\n");

    if (run_child(argv[0], argv, &st, &e) < 0)
        return 1;
    if (e != ENOEXEC) {
        printf("XLATE_EXEC: toggle FAIL: execve while disabled: expected "
               "ENOEXEC, got %s (exit %d)\n",
               e ? strerror(e) : "no error", st);
        return 1;
    }
    printf("XLATE_EXEC: toggle: step2 execve while disabled PASS (ENOEXEC)\n");

    if (write_switch("1") != 0) {
        printf("XLATE_EXEC: toggle FAIL: write 1: %s\n", strerror(errno));
        return 1;
    }
    v = read_enabled_or_die("after write 1");
    if (v < 0)
        return 1;
    if (v != 1) {
        printf("XLATE_EXEC: toggle FAIL: after write 1 the node reads %d\n", v);
        return 1;
    }
    printf("XLATE_EXEC: toggle: step3 wrote 1, node reads enabled=1\n");

    if (run_child(argv[0], argv, &st, &e) < 0)
        return 1;
    if (e != 0 || st != XLATE_PROBE_EXIT) {
        printf("XLATE_EXEC: toggle FAIL: execve after re-enable: %s exit %d\n",
               e ? strerror(e) : "no error", st);
        return 1;
    }
    printf("XLATE_EXEC: toggle: step3 execve after re-enable PASS (exit=%d)\n", st);

    /* Junk that must be refused.  Note what is *not* here: the empty string.  A
 * zero-length write carries no value, so it never reaches the handler and is
 * a no-op -- which is also how the neighbouring procfs tunables behave
 * (anonprov's atoi("") is 0, and 0 is in its range).  Asserting EINVAL there
 * would be asserting a convention this kernel does not have. */
static const char *const bad[] = { "2", "on", "01", " 1x", "1 1" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        /* write_switch returns a positive errno, not a negative one: it is
         * reporting what open()/write() left in errno, not a kernel return
         * value, and comparing it against -EINVAL would let a real rejection
         * read as a failure. */
        int rc = write_switch(bad[i]);
        if (rc != EINVAL) {
            printf("XLATE_EXEC: toggle FAIL: write \"%s\" returned %s, "
                   "expected EINVAL\n", bad[i],
                   rc ? strerror(rc) : "success");
            return 1;
        }
    }
    v = read_enabled_or_die("after rejected writes");
    if (v < 0)
        return 1;
    if (v != 1) {
        printf("XLATE_EXEC: toggle FAIL: a rejected write changed the switch "
               "to %d\n", v);
        return 1;
    }
    printf("XLATE_EXEC: toggle: step4 junk writes rejected EINVAL, still "
           "enabled=1\n");

    printf("XLATE_EXEC: toggle PASS\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: xlate_exec {ok|enoexec|unconfigured|foreign|script|toggle|run} "
                "<path> [args...]\n"
                "       xlate_exec stage <e_machine> <path>\n");
        return 2;
    }

    const char *mode = argv[1];

    if (strcmp(mode, "toggle") == 0)
        return mode_toggle(&argv[2]);

    /* stage needs three arguments of its own and execs nothing. */
    if (strcmp(mode, "stage") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: xlate_exec stage <e_machine> <path>\n");
            return 2;
        }
        return mode_stage(&argv[2]);
    }

    if (strcmp(mode, "run") == 0)
        return mode_run(&argv[2], argc - 2);

    /* The child's argv starts at the path: argv[0] is the program name,
     * so it must not be the mode word. */
    if (strcmp(mode, "ok") == 0)
        return mode_ok(&argv[2]);

    if (strcmp(mode, "enoexec") == 0) {
        /* Always exercise our own fixture: the point is that the kernel
         * rejects a file the hook must not touch, whatever the caller
         * named. */
        if (write_file("/tmp/xlate_corrupt.elf",
                       CORRUPT_ELF, sizeof(CORRUPT_ELF) - 1) < 0)
            return 1;
        char *corrupt_argv[] = { (char *)"/tmp/xlate_corrupt.elf", NULL };
        return mode_enoexec(corrupt_argv, "enoexec");
    }

    if (strcmp(mode, "script") == 0) {
        if (write_file("/tmp/xlate_shebang.sh",
                       SHEBANG_SCRIPT, sizeof(SHEBANG_SCRIPT) - 1) < 0)
            return 1;
        char *script_argv[] = { (char *)"/tmp/xlate_shebang.sh", NULL };
        return mode_script(script_argv);
    }

    /* Takes the caller's path verbatim -- that is the whole difference from
     * enoexec and unconfigured. */
    if (strcmp(mode, "foreign") == 0) {
        if (access(argv[2], F_OK) != 0) {
            printf("XLATE_EXEC: foreign FAIL: %s does not exist, so ENOEXEC "
                   "from execve would prove nothing\n", argv[2]);
            return 1;
        }
        return mode_enoexec(&argv[2], "foreign");
    }

    /* Like enoexec, this ignores <path>: the fixture is the point, and the
     * caller's path would only get in the way. */
    if (strcmp(mode, "unconfigured") == 0) {
        if (write_file("/tmp/xlate_unconfigured.elf",
                       UNCONFIGURED_ELF, sizeof(UNCONFIGURED_ELF) - 1) < 0)
            return 1;
        char *unconfigured_argv[] = { (char *)"/tmp/xlate_unconfigured.elf", NULL };
        return mode_enoexec(unconfigured_argv, "unconfigured");
    }

    fprintf(stderr, "xlate_exec: unknown mode '%s'\n", mode);
    return 2;
}