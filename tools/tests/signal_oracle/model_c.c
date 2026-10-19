/* Independent C transcription of the signal decision logic for host
 * cross-checking of the free-support oracle vectors.  This is a second
 * encoding of the semantics read from the frozen C sources; it is evidence
 * that the catalog is internally consistent, NOT production validation.
 *
 * Build (private): gcc -std=gnu11 -O0 model_c.c -o model-c
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#define DFL 0
#define IGN 1
#define H 2
#define NSIG 64

typedef struct {
    uint64_t pending, thread_pending, blocked;
    int kind[NSIG];
    uint32_t flags[NSIG];
    uint64_t smask[NSIG];
    int proc_has[NSIG];
    const char *proc_info[NSIG];
    int thr_obj;
    int thr_has[NSIG];
    const char *thr_info[NSIG];
    int ss_active;
    uint64_t ss_old;
    int sig_handling;
    uint64_t sig_old_blocked;
    uint64_t tmp_old;
    uint64_t sfd_mask;
    uint64_t ht_pending, ht_blocked, nat_pending, nat_blocked;
} St;

typedef struct {
    char pending[256], thread[256], blocked[256];
    char ev[256]; int have_ev;
    char ev_seq[1024]; int have_ev_seq;
    char res[32];
    char info[32];
    char rec_seq[1024]; int have_rec_seq;
    char poll_seq[512]; int have_poll_seq;
    char mseq[512]; int have_mseq;
    char act[512];
    char tmp_old[256];
    char natmask[256], htblock[256];
    char nres[256];
    char nres_seq[512]; int have_nres_seq;
    char npending[256], nht[256], ntake[256];
    char restart[8];
    char parked[8];
    char wake_seq[64]; int have_wake_seq;
} Obs;

static St g;
static Obs o;

static uint64_t bit(int s) { return 1ULL << s; }

static char *sigl(uint64_t m, char *buf)
{
    buf[0] = 0;
    for (int s = 1; s < NSIG; s++) {
        if (!(m & bit(s)))
            continue;
        if (buf[0])
            strcat(buf, ",");
        char tmp[8];
        sprintf(tmp, "%d", s);
        strcat(buf, tmp);
    }
    if (!buf[0])
        strcpy(buf, "-");
    return buf;
}

static void seq_append(char *buf, int *have, const char *part)
{
    if (*have)
        strcat(buf, ";");
    strcat(buf, part);
    *have = 1;
}

static int default_ignore(int s)
{
    return s == 17 || s == 23 || s == 28 || s == 18;
}
static int default_stop(int s)
{
    return s == 19 || s == 20 || s == 21 || s == 22;
}
static int core_default(int s)
{
    return s == 3 || s == 4 || s == 6 || s == 7 || s == 8 || s == 11 || s == 31;
}
static int default_terminate(int s)
{
    return !default_ignore(s) && !default_stop(s);
}

static int lowest(uint64_t m)
{
    for (int s = 1; s < NSIG; s++)
        if (m & bit(s))
            return s;
    return 0;
}

static void clear1(int sig)
{
    uint64_t b = bit(sig);
    if (g.thread_pending & b) {
        g.thread_pending &= ~b;
        if (g.thr_obj)
            g.thr_has[sig] = 0;
    } else {
        g.pending &= ~b;
        g.proc_has[sig] = 0;
    }
}

static const char *peek1(int sig)
{
    if (g.thread_pending & bit(sig)) {
        if (g.thr_obj && g.thr_has[sig])
            return g.thr_info[sig];
        return NULL;
    }
    if (g.proc_has[sig])
        return g.proc_info[sig];
    return NULL;
}

static const char *take1(int sig)
{
    const char *info = peek1(sig);
    clear1(sig);
    return info;
}

static uint64_t deliverable(void)
{
    return (g.pending | g.thread_pending) & ~g.blocked;
}

static void generation(int sig)
{
    if (sig == 18) {
        int stops[4] = {19, 20, 21, 22};
        for (int i = 0; i < 4; i++) {
            clear1(stops[i]);
            clear1(stops[i]);
        }
    } else if (default_stop(sig)) {
        clear1(18);
        clear1(18);
    }
}

static int discarded_at_queue(int sig)
{
    int kind = g.kind[sig];
    return sig != 18 && (kind == IGN || (kind == DFL && default_ignore(sig)));
}

static void op_action(int sig, int kind, uint32_t flags, uint64_t mask)
{
    g.kind[sig] = kind;
    g.flags[sig] = flags;
    g.smask[sig] = mask;
}

static void op_queue(int thr, int sig, const char *info)
{
    generation(sig);
    if (discarded_at_queue(sig))
        return;
    if (thr) {
        if (info && !g.thr_obj)
            g.thr_obj = 1;
        if (!(g.thread_pending & bit(sig)) && g.thr_obj) {
            g.thr_has[sig] = info != NULL;
            if (info)
                g.thr_info[sig] = info;
        }
        g.thread_pending |= bit(sig);
    } else {
        if (info) {
            g.proc_has[sig] = 1;
            g.proc_info[sig] = info;
        } else {
            g.proc_has[sig] = 0;
        }
        g.pending |= bit(sig);
    }
}

/* returns 0 none, 1 drop, 2 stop, 3 exit, 4 handler */
static int step_deliver(int kernel, char *evbuf)
{
    uint64_t d = deliverable();
    if (!d)
        return 0;
    int sig = lowest(d);
    int kind = g.kind[sig];
    uint32_t flags = g.flags[sig];
    uint64_t smask = g.smask[sig];
    if (kind == IGN || (kind == DFL && default_ignore(sig))) {
        clear1(sig);
        if (g.ss_active) {
            g.blocked = g.ss_old;
            g.ss_active = 0;
        }
        sprintf(evbuf, "drop:%d", sig);
        return 1;
    }
    if (kind == DFL && default_stop(sig)) {
        clear1(sig);
        if (g.ss_active) {
            g.blocked = g.ss_old;
            g.ss_active = 0;
        }
        sprintf(evbuf, "stop:%d", sig);
        return 2;
    }
    if (kind == DFL) {
        clear1(sig);
        sprintf(evbuf, "exit:%d:%s", sig,
                (core_default(sig) && !kernel) ? "core" : "nocore");
        return 3;
    }
    const char *info = peek1(sig);
    clear1(sig);
    if (flags & 0x80000000U)
        g.kind[sig] = DFL;
    uint64_t old = g.ss_active ? g.ss_old : g.blocked;
    g.sig_old_blocked = old;
    g.ss_active = 0;
    g.blocked |= smask;
    if (!(flags & 0x40000000U))
        g.blocked |= bit(sig);
    g.sig_handling = sig;
    {
        char mb[256];
        sprintf(evbuf, "handler:%d:%s:old=%s", sig, info ? info : "absent", sigl(old, mb));
    }
    return 4;
}

static void deliver_call(int kernel)
{
    char buf[1024];
    buf[0] = 0;
    for (;;) {
        char one[300];
        int r = step_deliver(kernel, one);
        if (!r)
            break;
        if (buf[0])
            strcat(buf, ";");
        strcat(buf, one);
        if (kernel) {
            if (r == 3)
                break;
        } else {
            if (r >= 2)
                break;
        }
    }
    if (!buf[0])
        strcpy(buf, "-");
    strcpy(o.ev, buf);
    o.have_ev = 1;
    seq_append(o.ev_seq, &o.have_ev_seq, buf);
}

static void op_deliver(void) { deliver_call(0); }
static void op_kdeliver(void) { deliver_call(1); }
static void op_boundary(void) { deliver_call(0); }

static void op_sigreturn(void)
{
    if (g.sig_handling) {
        g.blocked = g.sig_old_blocked;
        g.sig_handling = 0;
    }
}

static void op_timedwait(uint64_t mask, int poll)
{
    uint64_t m = (g.pending | g.thread_pending) & mask;
    if (m) {
        int sig = lowest(m);
        const char *info = take1(sig);
        sprintf(o.res, "%d", sig);
        strcpy(o.info, info ? info : "sigonly");
    } else if (poll) {
        strcpy(o.res, "-11");
    }
}

static void op_maskop(int how, uint64_t sset)
{
    uint64_t m = sset & ~(bit(9) | bit(19));
    char b[256];
    if (how == 0)
        g.blocked |= m;
    else if (how == 1)
        g.blocked &= ~m;
    else if (how == 2)
        g.blocked = m;
    else
        strcpy(o.res, "-22");
    seq_append(o.mseq, &o.have_mseq, sigl(g.blocked, b));
}

static void op_maskop_none(int how)
{
    (void)how;
    strcpy(o.res, "0");
}

static void op_sigsuspend(uint64_t sset)
{
    g.ss_old = g.blocked;
    g.ss_active = 1;
    g.blocked = sset & ~(bit(9) | bit(19));
    uint64_t d = deliverable();
    int has = 0;
    for (int s = 1; s < NSIG; s++) {
        if (!(d & bit(s)))
            continue;
        if (g.kind[s] == IGN || (g.kind[s] == DFL && default_ignore(s)))
            continue;
        has = 1;
        break;
    }
    strcpy(o.res, "-4");
    strcpy(o.parked, has ? "0" : "1");
}

static void op_wake(void) {}
static void op_restore(void)
{
    if (g.ss_active && !g.sig_handling) {
        g.blocked = g.ss_old;
        g.ss_active = 0;
    }
}

static void op_exec(void)
{
    for (int s = 1; s < NSIG; s++) {
        if (g.kind[s] != IGN && g.kind[s] != DFL)
            g.kind[s] = DFL;
        g.flags[s] = 0;
        g.smask[s] = 0;
    }
    g.pending = 0;
    g.thread_pending = 0;
    memset(g.proc_has, 0, sizeof(g.proc_has));
    memset(g.thr_has, 0, sizeof(g.thr_has));
    g.sig_handling = 0;
    g.ss_active = 0;
}

static const char *kind_name(int k)
{
    return k == DFL ? "DFL" : k == IGN ? "IGN" : "H";
}

static void op_actions(int sig, ...)
{
    (void)sig;
    /* fixed-shape emitter: op_actions(s1, s2, 0) */
    va_list ap;
    va_start(ap, sig);
    char buf[512];
    buf[0] = 0;
    int args[8]; int n = 0;
    args[n++] = sig;
    while (n < 8) {
        int v = va_arg(ap, int);
        if (v == 0)
            break;
        args[n++] = v;
    }
    va_end(ap);
    for (int i = 0; i < n; i++) {
        int s = args[i];
        char mb[256];
        char part[300];
        sprintf(part, "%d:%s:%u:%s", s, kind_name(g.kind[s]), g.flags[s],
                g.smask[s] ? sigl(g.smask[s], mb) : "0");
        if (buf[0])
            strcat(buf, ";");
        strcat(buf, part);
    }
    strcpy(o.act, buf);
}

static void op_tmpmask(uint64_t sset)
{
    char b[256];
    g.tmp_old = g.blocked;
    g.blocked = sset & ~(bit(9) | bit(19));
    strcpy(o.tmp_old, sigl(g.tmp_old, b));
    seq_append(o.mseq, &o.have_mseq, sigl(g.blocked, b));
}

static void op_tmpres(void)
{
    char b[256];
    g.blocked = g.tmp_old;
    seq_append(o.mseq, &o.have_mseq, sigl(g.blocked, b));
}

static void op_sfdcreate(uint64_t mask) { g.sfd_mask = mask; }
static void op_sfdmask(uint64_t mask) { g.sfd_mask = mask; }

static void op_sfdread(int count, int nonblock)
{
    if (count < 128) {
        seq_append(o.rec_seq, &o.have_rec_seq, "EINVAL");
        return;
    }
    char buf[512];
    buf[0] = 0;
    int nrec = 0;
    for (;;) {
        uint64_t m = (g.pending | g.thread_pending) & g.sfd_mask;
        if (!m) {
            if (nrec)
                seq_append(o.rec_seq, &o.have_rec_seq, buf);
            else if (nonblock)
                seq_append(o.rec_seq, &o.have_rec_seq, "EAGAIN");
            else
                seq_append(o.rec_seq, &o.have_rec_seq, "park");
            return;
        }
        int sig = lowest(m);
        const char *info = take1(sig);
        char part[64];
        sprintf(part, "%d:%s", sig, info ? info : "absent");
        if (nrec)
            strcat(buf, ";");
        strcat(buf, part);
        nrec++;
        if ((nrec + 1) * 128 > count) {
            seq_append(o.rec_seq, &o.have_rec_seq, buf);
            return;
        }
    }
}

static void op_sfdpoll(void)
{
    seq_append(o.poll_seq, &o.have_poll_seq,
               ((g.pending | g.thread_pending) & g.sfd_mask) ? "1" : "0");
}

static void op_restart(void)
{
    uint64_t d = deliverable();
    int restart = d != 0;
    for (int s = 1; s < NSIG && restart; s++) {
        if (!(d & bit(s)))
            continue;
        if (g.kind[s] == IGN || (g.kind[s] == DFL && default_ignore(s)))
            continue;
        if (g.kind[s] != DFL && !(g.flags[s] & 0x10000000U))
            restart = 0;
    }
    strcpy(o.restart, restart ? "1" : "0");
}

static void op_np(int thr, int sig)
{
    if (thr)
        g.nat_pending |= bit(sig);
    else
        g.ht_pending |= bit(sig);
}

static void op_ncheck(int mode, uint64_t selected)
{
    uint64_t pending = g.ht_pending | g.nat_pending;
    uint64_t d;
    if (mode & 4) {
        d = pending & selected;
        if (!(mode & 2))
            d &= (0 - d);
    } else {
        d = pending & ~g.nat_blocked;
    }
    if (!(mode & 2)) {
        uint64_t local = d & g.nat_pending;
        g.nat_pending &= ~local;
        g.ht_pending &= ~(d & ~local);
    }
    char b[256];
    strcpy(o.nres, sigl(d, b));
    seq_append(o.nres_seq, &o.have_nres_seq, b);
}

static void op_nmask(uint64_t mask)
{
    g.nat_blocked = mask & ~(bit(9) | bit(19) | 1ULL);
    char b[256];
    strcpy(o.natmask, sigl(g.nat_blocked, b));
}

static void op_nblock(uint64_t mask)
{
    g.ht_blocked = mask & ~bit(9);
    char b[256];
    strcpy(o.htblock, sigl(g.ht_blocked, b));
}

static void op_ntake(void)
{
    uint64_t d = g.ht_pending & ~g.ht_blocked;
    g.ht_pending &= g.ht_blocked;
    char b[256];
    strcpy(o.ntake, sigl(d, b));
}

static void op_nstate(void)
{
    char b[256];
    strcpy(o.npending, sigl(g.nat_pending, b));
    strcpy(o.nht, sigl(g.ht_pending, b));
}

#include "vectors_gen.c"

static void reset_state(void)
{
    memset(&g, 0, sizeof(g));
    memset(&o, 0, sizeof(o));
}

static void finish_state(void)
{
    char b[256];
    strcpy(o.pending, sigl(g.pending, b));
    strcpy(o.thread, sigl(g.thread_pending, b));
    strcpy(o.blocked, sigl(g.blocked, b));
}

static void print_obs(const char *id)
{
    if (o.have_ev)
        printf("%s\tev\t%s\n", id, o.ev);
    if (o.have_ev_seq)
        printf("%s\tev_seq\t%s\n", id, o.ev_seq);
    if (o.have_mseq)
        printf("%s\tmseq\t%s\n", id, o.mseq);
    if (o.have_rec_seq)
        printf("%s\trec_seq\t%s\n", id, o.rec_seq);
    if (o.have_poll_seq)
        printf("%s\tpoll_seq\t%s\n", id, o.poll_seq);
    if (o.have_nres_seq)
        printf("%s\tnres_seq\t%s\n", id, o.nres_seq);
    if (o.res[0])
        printf("%s\tres\t%s\n", id, o.res);
    if (o.info[0])
        printf("%s\tinfo\t%s\n", id, o.info);
    if (o.restart[0])
        printf("%s\trestart\t%s\n", id, o.restart);
    if (o.parked[0])
        printf("%s\tparked\t%s\n", id, o.parked);
    if (o.act[0])
        printf("%s\tact\t%s\n", id, o.act);
    if (o.tmp_old[0])
        printf("%s\ttmp_old\t%s\n", id, o.tmp_old);
    if (o.natmask[0])
        printf("%s\tnatmask\t%s\n", id, o.natmask);
    if (o.htblock[0])
        printf("%s\thtblock\t%s\n", id, o.htblock);
    if (o.nres[0])
        printf("%s\tnres\t%s\n", id, o.nres);
    if (o.npending[0])
        printf("%s\tnpending\t%s\n", id, o.npending);
    if (o.nht[0])
        printf("%s\tnht\t%s\n", id, o.nht);
    if (o.ntake[0])
        printf("%s\tntake\t%s\n", id, o.ntake);
    printf("%s\tpending\t%s\n", id, o.pending);
    printf("%s\tthread\t%s\n", id, o.thread);
    printf("%s\tblocked\t%s\n", id, o.blocked);
}

int main(void)
{
    for (int i = 0; g_vectors[i].id; i++) {
        reset_state();
        g_vectors[i].run();
        finish_state();
        print_obs(g_vectors[i].id);
    }
    return 0;
}
