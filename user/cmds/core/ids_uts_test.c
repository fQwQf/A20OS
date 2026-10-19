/*
 * Guest coverage for the process-group/session and UTS-name policy: the
 * getpgid/getsid lookups, setpgid's normalisation and child/session rules,
 * setsid's leader rule, and the sethostname/setdomainname length and authority
 * rules.  Every printed value is deterministic for a given boot, so a C and a
 * Rust kernel can be compared on the same trace.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail(const char *what)
{
    printf("IDS_UTS: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int test_ids(void)
{
    pid_t pid = getpid();
    pid_t pgid = getpgid(0);
    pid_t sid = getsid(0);
    if (pgid <= 0 || sid <= 0)
        return fail("lookup");
    printf("IDS_UTS: lookup PASS pid=%d pgid=%d sid=%d\n", pid, pgid, sid);

    /* setpgid(0, 0) makes this process its own group leader. */
    if (setpgid(0, 0) < 0)
        return fail("setpgid-self");
    if (getpgid(0) != pid)
        return fail("setpgid-self-read");
    printf("IDS_UTS: setpgid-self PASS pgid=%d\n", getpgid(0));

    errno = 0;
    if (setpgid(0, -1) == 0 || errno != EINVAL)
        return fail("setpgid-negative");
    printf("IDS_UTS: setpgid-negative EINVAL\n");

    /* A task in another session cannot be moved by us.  The child reports
     * readiness from a pipe only after setsid() returned, so the check below
     * runs after the session change rather than racing it. */
    int ready[2];
    if (pipe(ready) < 0)
        return fail("pipe");
    pid_t child = fork();
    if (child < 0)
        return fail("fork");
    if (child == 0) {
        if (setsid() < 0)
            _exit(1);
        char r = 'R';
        (void)write(ready[1], &r, 1);
        for (;;)
            pause();
    }
    close(ready[1]);
    char r = 0;
    if (read(ready[0], &r, 1) != 1 || r != 'R') {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        return fail("setsid-ready");
    }
    close(ready[0]);
    int status = 0;
    errno = 0;
    if (setpgid(child, child) == 0 || errno != EPERM) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        return fail("setpgid-other-session");
    }
    printf("IDS_UTS: setpgid-other-session EPERM\n");
    kill(child, SIGKILL);
    waitpid(child, &status, 0);

    /* setsid in a child: the first call succeeds, the second is refused. */
    child = fork();
    if (child < 0)
        return fail("fork-setsid");
    if (child == 0) {
        pid_t sid2 = setsid();
        if (sid2 != getpid())
            _exit(2);
        errno = 0;
        if (setsid() != -1 || errno != EPERM)
            _exit(3);
        _exit(0);
    }
    if (waitpid(child, &status, 0) != child)
        return fail("wait-setsid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail("setsid-status");
    printf("IDS_UTS: setsid PASS\n");

    /* A non-child cannot be moved. */
    errno = 0;
    if (setpgid(1, 1) == 0 || errno != ESRCH)
        return fail("setpgid-nonchild");
    printf("IDS_UTS: setpgid-nonchild ESRCH\n");
    return 0;
}

static int test_uts(void)
{
    struct utsname u;
    if (sethostname("a20ids", 6) < 0)
        return fail("sethostname");
    if (uname(&u) < 0)
        return fail("uname");
    if (strcmp(u.nodename, "a20ids") != 0)
        return fail("hostname-read");
    printf("IDS_UTS: hostname PASS %s\n", u.nodename);

    if (setdomainname("a20dom", 6) < 0)
        return fail("setdomainname");
    if (uname(&u) < 0)
        return fail("uname-domain");
    if (strcmp(u.domainname, "a20dom") != 0)
        return fail("domainname-read");
    printf("IDS_UTS: domainname PASS %s\n", u.domainname);

    errno = 0;
    if (sethostname(NULL, 0) == 0 || errno != EFAULT)
        return fail("sethostname-null");
    char longname[70];
    memset(longname, 'x', sizeof(longname));
    errno = 0;
    if (sethostname(longname, sizeof(longname)) == 0 || errno != EINVAL)
        return fail("sethostname-long");
    printf("IDS_UTS: bounds PASS\n");
    return 0;
}

int main(void)
{
    printf("IDS_UTS: start\n");
    if (test_ids())
        return 1;
    if (test_uts())
        return 1;
    printf("IDS_UTS: PASS\n");
    return 0;
}
