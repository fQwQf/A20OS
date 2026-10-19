/*
 * Socket vfile lifecycle: create/close/reuse balance and the install-failure
 * rollback, checked against the kernel's own reference ledger.
 *
 * The close teardown and the install rollback are the transactions this test
 * exists for.  It drives them through the real syscalls and then reads
 * /proc/net/status: the ledger's `live` must return to where it started, its
 * `faults` must not move (a double free or a release of a non-socket), and the
 * failure path must not leave a socket allocated with no owner.
 *
 * The fd-exhaustion phase is best-effort: if the table does not refuse the
 * creates within the bound, the run reports the ceiling it reached and still
 * checks the balance, because a test that silently stopped testing would be
 * worse than one that says what it covered.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_FDS 2048

static int failures;
static int checks;

static void ok(int cond, const char *fmt, ...)
{
    checks++;
    if (!cond)
        failures++;
    printf("SOCK_LIFECYCLE: %s ", cond ? "ok  " : "FAIL");
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

typedef struct {
    int live;
    int allocs;
    int frees;
    int faults;
    int valid;
} ledger_t;

static ledger_t read_ledger(void)
{
    ledger_t l = { 0, 0, 0, 0, 0 };
    char buf[8192];
    int fd = open("/proc/net/status", O_RDONLY);
    if (fd < 0)
        return l;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return l;
    buf[n] = '\0';
    char *p = strstr(buf, "net_sock_ref:");
    if (!p)
        return l;
    if (sscanf(p, "net_sock_ref: live=%d allocs=%d frees=%d faults=%d",
               &l.live, &l.allocs, &l.frees, &l.faults) == 4)
        l.valid = 1;
    return l;
}

int main(void)
{
    ledger_t start = read_ledger();
    ok(start.valid, "the reference ledger is readable "
       "(live=%d allocs=%d frees=%d faults=%d)",
       start.live, start.allocs, start.frees, start.faults);
    if (!start.valid) {
        printf("SOCK_LIFECYCLE: FAIL no ledger row in /proc/net/status\n");
        return 1;
    }

    /* Phase 1: create a batch, close half, reuse the slots, close the rest. */
    enum { BATCH = 64 };
    int fds[BATCH];
    int made = 0;
    for (int i = 0; i < BATCH; i++) {
        fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (fds[i] < 0)
            break;
        made++;
    }
    ok(made == BATCH, "created %d/%d sockets", made, BATCH);

    ledger_t after_open = read_ledger();
    ok(after_open.live - start.live == made,
       "the ledger counts every open socket (live %d -> %d, made %d)",
       start.live, after_open.live, made);
    ok(after_open.faults == start.faults, "opening moved no fault counter");

    for (int i = 0; i < made; i += 2)
        ok(close(fds[i]) == 0, "close even fd %d", i);
    ledger_t after_half = read_ledger();
    ok(after_half.live == start.live + made - (made + 1) / 2,
       "closing half released exactly that many (live=%d)", after_half.live);

    int remade = 0;
    for (int i = 0; i < made; i += 2) {
        fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (fds[i] < 0)
            break;
        remade++;
    }
    ok(remade == (made + 1) / 2, "re-created %d/%d closed sockets", remade,
       (made + 1) / 2);
    ledger_t after_reuse = read_ledger();
    ok(after_reuse.live == start.live + made,
       "reuse did not leak (live=%d expected=%d)", after_reuse.live,
       start.live + made);

    for (int i = 0; i < made; i++)
        close(fds[i]);
    ledger_t after_close = read_ledger();
    ok(after_close.live == start.live,
       "closing everything returned the ledger (live=%d expected=%d)",
       after_close.live, start.live);
    ok(after_close.faults == start.faults,
       "the whole cycle moved no fault counter (faults=%d)", after_close.faults);

    /* Phase 2: drive the install-failure path.  When the fd table or the socket
     * table refuses a create, the socket that was built for it must be torn down
     * and its descriptor never installed; the ledger is the proof. */
    int *many = calloc(MAX_FDS, sizeof(int));
    if (!many)
        return 1;
    int opened = 0;
    int refused_errno = 0;
    for (int i = 0; i < MAX_FDS; i++) {
        many[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (many[i] < 0) {
            refused_errno = errno;
            break;
        }
        opened++;
    }
    ledger_t at_ceiling = read_ledger();
    ok(at_ceiling.faults == start.faults,
       "the refusal path moved no fault counter (faults=%d)", at_ceiling.faults);
    ok(at_ceiling.live - after_close.live == opened,
       "the ledger matches the sockets that really opened (live delta=%d "
       "opened=%d)", at_ceiling.live - after_close.live, opened);
    if (refused_errno)
        printf("SOCK_LIFECYCLE: info create refused after %d sockets with "
               "errno=%d\n", opened, refused_errno);
    else
        printf("SOCK_LIFECYCLE: info no refusal within %d sockets; the "
               "rollback path was not reached in this run\n", MAX_FDS);

    for (int i = 0; i < opened; i++)
        close(many[i]);
    free(many);

    ledger_t end = read_ledger();
    ok(end.live == start.live, "final live returned to the baseline "
       "(live=%d expected=%d)", end.live, start.live);
    ok(end.faults == start.faults, "final faults unchanged (faults=%d)",
       end.faults);

    if (failures) {
        printf("SOCK_LIFECYCLE: FAIL (%d of %d checks failed)\n", failures,
               checks);
        return 1;
    }
    printf("SOCK_LIFECYCLE: PASS (%d checks)\n", checks);
    return 0;
}
