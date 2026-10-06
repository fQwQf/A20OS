/*
 * conntrack capacity and idle-timeout gate.
 *
 * Run by tools/targets-smoke.mk:smoke-ct-capacity, which boots the same
 * riscv64 dev image as smoke-netfilter-nat and runs this instead of it.
 *
 * What it proves, and why it exists rather than more packet traffic:
 *
 *   1. The table fills to exactly its reported capacity and holds there.
 *      `ct_capacity` is read back from /proc rather than compiled in here, so
 *      the assertion is against the tier the image was actually built with
 *      (64 / 256 / 1024 by net_profile.h), not against a number this file
 *      copied.
 *   2. One flow past capacity evicts exactly one entry, and the entry evicted
 *      is the *least recently used* one -- asserted twice, on both sides of
 *      the eviction.  Before the overflow the victim is the entry inserted
 *      first; after it, the victim is the entry inserted second.  A gate that
 *      only asserted "ct_evicted moved" would pass an implementation that
 *      dropped an arbitrary entry, which is the failure that loses the flow
 *      you are looking at.
 *   3. The idle sweeper reclaims an entry once its timeout has passed, and
 *      does not reclaim it before.  The timeout is shortened through the
 *      cttimeout verb rather than by waiting out the compiled-in 30 s.
 *
 * Step 3's wait is a bounded poll rather than one sleep: the sweeper is gated
 * on a one-second interval in a20_lwip_poll_timers_locked
 * (kernel/net/lwip_stack.c) and re-arms the CPU timer from the proc wheel, so
 * how many ticks land inside a given wall-clock window is the guest's business.
 * The assertions are on the outcome (reclaimed, counted as a timeout, and a
 * sweep ran) with a deadline, not on the tick count.
 *
 * The sleep between the first two insertions is what makes the LRU key
 * distinct: netfilter_ct_lru() breaks a tie by slot index, so two entries
 * inserted in the same tick would make the assertion test the tie-break rather
 * than the ordering.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NF_PATH "/proc/a20/netfilter"

/* Inside 192.168.77.0/24, a range the guest has no route to; nothing here ever
 * puts a packet on the wire, the tuples only need to be distinct. */
#define INJECT_SRC "192.168.77.1"
#define INJECT_DST "8.8.8.8"
#define FIRST_PORT 40000

/* The sweeper runs on a one-second interval (lwip_stack.c) and how many ticks
 * land inside a wall-clock window is the guest's business, not this test's, so
 * the idle wait is a bounded poll rather than one sleep.  24 x 500 ms is
 * generous for a limit of 100 ms: it turns a slow host into a slower pass, not
 * into a flake, while still failing a sweeper that never runs. */
#define IDLE_POLL_TRIES 24
#define IDLE_POLL_INTERVAL_US 500000

#define CHK(cond, msg)                                                         \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("CTCAP_TEST: FAIL %s:%d %s (errno=%d %s)\n", __func__,      \
                   __LINE__, (msg), errno, strerror(errno));                   \
            return 1;                                                          \
        } else {                                                               \
            printf("CTCAP_TEST: ok %s\n", (msg));                              \
        }                                                                      \
    } while (0)

static int nf_write(const char *s) {
    int fd = open(NF_PATH, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, s, strlen(s));
    int e = (n == (ssize_t)strlen(s)) ? 0 : -1;
    close(fd);
    return e;
}

static int nf_read(char *buf, size_t bufsz) {
    int fd = open(NF_PATH, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return (int)n;
}

static long long nf_stat(const char *text, const char *key) {
    size_t klen = strlen(key);
    const char *p = text;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == ':') {
            const char *v = p + klen + 1;
            while (*v == ' ')
                v++;
            return strtoll(v, NULL, 10);
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return -1;
}

static int inject(unsigned sport, unsigned proto, unsigned state) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "ctinject " INJECT_SRC " " INJECT_DST " %u %u %u %u", sport,
             sport + 1, proto, state);
    return nf_write(cmd);
}

int main(void) {
    char buf[4096];

    CHK(nf_write("ctflush") == 0, "flush the conntrack table");
    CHK(nf_write("cttimeout 0 0 0") == 0, "restore the compiled-in timeouts");
    CHK(nf_read(buf, sizeof(buf)) > 0, "read the netfilter node");
    long long capacity = nf_stat(buf, "ct_capacity");
    CHK(capacity > 1, "the image reports a conntrack capacity");
    CHK(nf_stat(buf, "ct_tracked") == 0, "the table is empty after a flush");
    CHK(nf_stat(buf, "ct_lru_victim") == -1, "an empty table has no victim");

    /* Rejected injections, so the gate cannot pass by inserting entries that
     * the packet path would never have created. */
    CHK(nf_write("ctinject " INJECT_SRC " " INJECT_DST " 40000 40001 1 0") < 0,
        "an injected proto the data plane never tracks is rejected");
    CHK(nf_write("ctinject " INJECT_SRC " " INJECT_DST " 40000 40001 6 7") < 0,
        "an injected state outside the two real ones is rejected");
    CHK(nf_write("ctinject 999.1.1.1 " INJECT_DST " 40000 40001 17 0") < 0,
        "a malformed address is rejected");
    CHK(nf_write("ctinject " INJECT_SRC " " INJECT_DST " 40000 40001 17") < 0,
        "a short injection is rejected");

    /* 1. Fill to capacity. */
    CHK(inject(FIRST_PORT, 17, 0) == 0, "inject the oldest flow");
    usleep(30000); /* distinct tick, so the LRU key is not a tie */
    for (unsigned i = 1; i < (unsigned)capacity; i++)
        CHK(inject(FIRST_PORT + i, 17, 0) == 0, "fill the table to capacity");

    CHK(nf_read(buf, sizeof(buf)) > 0, "read after filling");
    CHK(nf_stat(buf, "ct_tracked") == capacity,
        "the table holds exactly capacity");
    CHK(nf_stat(buf, "ct_evicted") == 0, "filling to capacity evicted nothing");
    CHK(nf_stat(buf, "ct_created") == capacity,
        "one entry created per injection");
    CHK(nf_stat(buf, "ct_lru_victim") == FIRST_PORT,
        "the next insert would evict the flow inserted first");

    /* 2. One flow past capacity. */
    CHK(inject(FIRST_PORT + (unsigned)capacity, 17, 0) == 0,
        "insert one flow past capacity");
    CHK(nf_read(buf, sizeof(buf)) > 0, "read after the overflow");
    CHK(nf_stat(buf, "ct_tracked") == capacity,
        "the table is still exactly capacity after an insert");
    CHK(nf_stat(buf, "ct_evicted") == 1,
        "the overflow evicted exactly one entry");
    CHK(nf_stat(buf, "ct_lru_victim") == FIRST_PORT + 1,
        "the evicted entry was the least recently used one, not an arbitrary "
        "one");

    /* 3. Idle timeout. */
    CHK(nf_write("ctflush") == 0, "flush before the timeout check");
    CHK(nf_write("cttimeout 100 100 100") == 0, "shorten the idle timeouts");
    CHK(inject(FIRST_PORT, 17, 0) == 0, "inject one flow with a 100ms timeout");
    CHK(nf_read(buf, sizeof(buf)) > 0, "read immediately after the injection");
    long long to0 = nf_stat(buf, "ct_timeout");
    CHK(nf_stat(buf, "ct_tracked") == 1, "the fresh entry is tracked");
    CHK(to0 >= 0, "the timeout counter is readable");
    long long sw0 = nf_stat(buf, "ct_sweeps");
    CHK(sw0 >= 0, "the sweep counter is readable");

    /*
     * The wait is a bounded poll rather than one sleep: how many ticks land
     * inside a given wall-clock window is the guest's business, and asserting
     * after a single fixed sleep would be asserting on QEMU's scheduling.
     */
    int reclaimed = 0;
    for (int i = 0; i < IDLE_POLL_TRIES; i++) {
        usleep(IDLE_POLL_INTERVAL_US);
        if (nf_read(buf, sizeof(buf)) <= 0)
            continue;
        if (nf_stat(buf, "ct_tracked") == 0) {
            reclaimed = 1;
            break;
        }
    }
    printf("CTCAP_TEST: idle poll: tracked=%lld timeout=%lld (was %lld) "
           "sweeps=%lld (was %lld)\n",
           nf_stat(buf, "ct_tracked"), nf_stat(buf, "ct_timeout"), to0,
           nf_stat(buf, "ct_sweeps"), sw0);
    CHK(reclaimed, "the idle entry was reclaimed inside the deadline");
    CHK(nf_stat(buf, "ct_timeout") == to0 + 1,
        "the reclaim was counted as a timeout, not as an eviction");
    CHK(nf_stat(buf, "ct_sweeps") > sw0, "a sweeper run is what reclaimed it");

    /* Leave the table as it was found: defaults in force, nothing tracked. */
    CHK(nf_write("cttimeout 0 0 0") == 0, "restore the compiled-in timeouts");
    CHK(nf_write("ctflush") == 0, "flush on the way out");

    printf("CTCAP_TEST: PASS capacity=%lld\n", capacity);
    return 0;
}
