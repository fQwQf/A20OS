/*
 * Paired C-vs-Rust microbenchmark for the signal decision core.
 *
 * Both sides run the same representative per-signal decision mix over an
 * identical precomputed input array so the comparison is of compiled code, not
 * of different work. Rounds alternate C->Rust and Rust->C to cancel ordering
 * bias. Fixed CPU/optimisation (host, -O2) for both. Driven by
 * tools/bench-signal-core-host.sh.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* production Rust ABI */
int      a20_signal_core_dump_default(int sig);
uint64_t a20_signal_mask_apply(int how, uint64_t old, uint64_t set);
uint64_t a20_signal_deliverable_mask(uint64_t pending, uint64_t thread_pending, uint64_t blocked);
int      a20_signal_lowest(uint64_t mask);
int      a20_signal_action_deliverable(uint64_t blocked_mask, uint64_t handler, int sig);
int      a20_signal_action_fatal(uint64_t blocked_mask, uint64_t handler, int sig);
int      a20_signal_queue_discard(int sig, uint64_t handler);
uint64_t a20_signal_generation_cancel_mask(int sig);
int      a20_signal_restart_vetoes(uint64_t handler, int flags);
int      a20_signal_default_stop(int sig);
uint64_t a20_signal_native_thread_check(uint64_t pending_ht, uint64_t pending_thread, uint64_t blocked, uint32_t mode, uint64_t selected, uint64_t *ct, uint64_t *ch);

/* C reference transcription of the same decisions */
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGKILL 9
#define SIGURG 23
#define SIGWINCH 28
static uint64_t kb(int s) { return (s > 0 && s < 64) ? (1ULL << s) : 0; }
static int c_default_stop(int sig) {
    switch (sig) { case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU: return 1; default: return 0; }
}
static int c_default_ignore(int sig) {
    switch (sig) { case SIGCHLD: case SIGURG: case SIGWINCH: case SIGCONT: return 1; default: return 0; }
}
static int c_default_terminate(int sig) {
    switch (sig) { case SIGCHLD: case SIGURG: case SIGWINCH: case SIGSTOP:
        case SIGTSTP: case SIGTTIN: case SIGTTOU: case SIGCONT: return 0; default: return 1; }
}
static uint64_t c_mask_apply(int how, uint64_t old, uint64_t set) {
    switch (how) { case 0: return old | set; case 1: return old & ~set; case 2: return set; default: return old; }
}
static uint64_t c_deliverable(uint64_t p, uint64_t tp, uint64_t b) { return (p | tp) & ~b; }
static int c_lowest(uint64_t mask) { for (int s = 1; s < 64; s++) if (mask & (1ULL << s)) return s; return 0; }
static int c_action_deliverable(uint64_t b, uint64_t h, int s) {
    if (b & kb(s)) return 0; if (h == 1) return 0; if (h == 0 && c_default_ignore(s)) return 0; return 1;
}
static int c_action_fatal(uint64_t b, uint64_t h, int s) {
    return c_action_deliverable(b, h, s) && h == 0 && c_default_terminate(s);
}
static int c_queue_discard(int s, uint64_t h) {
    if (s == SIGCONT) return 0; if (h == 1) return 1; if (h == 0 && c_default_ignore(s)) return 1; return 0;
}
static uint64_t c_gen_cancel(int s) {
    if (s == SIGCONT) return kb(SIGSTOP) | kb(SIGTSTP) | kb(SIGTTIN) | kb(SIGTTOU);
    if (c_default_stop(s)) return kb(SIGCONT); return 0;
}
static int c_restart_vetoes(uint64_t h, int f) { return h != 0 && h != 1 && !(f & 0x10000000); }
static uint64_t c_native_thread_check(uint64_t ph, uint64_t pt, uint64_t b, uint32_t mode,
                                      uint64_t sel, uint64_t *ct, uint64_t *ch) {
    uint64_t pending = ph | pt, deliver = pending & ~b;
    if (mode & 4) { deliver = pending & sel; if (!(mode & 2)) deliver &= 0ULL - deliver; }
    *ct = *ch = 0;
    if (!(mode & 2)) { uint64_t local = deliver & pt; *ct = local; *ch = deliver & ~local; }
    return deliver;
}

#define N 400000
struct in { int sig; int how; uint64_t handler; int flags; uint32_t mode; uint64_t a, b; };
static struct in in[N];

typedef uint64_t (*work_fn)(void);

static uint64_t work_rust(void) {
    uint64_t acc = 0, ct, ch;
    for (int i = 0; i < N; i++) {
        const struct in *p = &in[i];
        acc += a20_signal_mask_apply(p->how, p->a, p->b);
        acc += a20_signal_deliverable_mask(p->a, p->b, p->a >> 1);
        acc += a20_signal_action_deliverable(p->a, p->handler, p->sig);
        acc += a20_signal_action_fatal(p->b, p->handler, p->sig);
        acc += a20_signal_lowest(p->a);
        acc += a20_signal_queue_discard(p->sig, p->handler);
        acc += a20_signal_generation_cancel_mask(p->sig);
        acc += a20_signal_restart_vetoes(p->handler, p->flags);
        acc += a20_signal_native_thread_check(p->a, p->b, p->a, p->mode, p->b, &ct, &ch);
        acc += ct + ch;
    }
    return acc;
}

static uint64_t work_c(void) {
    uint64_t acc = 0, ct, ch;
    for (int i = 0; i < N; i++) {
        const struct in *p = &in[i];
        acc += c_mask_apply(p->how, p->a, p->b);
        acc += c_deliverable(p->a, p->b, p->a >> 1);
        acc += c_action_deliverable(p->a, p->handler, p->sig);
        acc += c_action_fatal(p->b, p->handler, p->sig);
        acc += c_lowest(p->a);
        acc += c_queue_discard(p->sig, p->handler);
        acc += c_gen_cancel(p->sig);
        acc += c_restart_vetoes(p->handler, p->flags);
        acc += c_native_thread_check(p->a, p->b, p->a, p->mode, p->b, &ct, &ch);
        acc += ct + ch;
    }
    return acc;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 12;
    uint64_t s = 0x123456789abcdefULL;
    for (int i = 0; i < N; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        struct in *p = &in[i];
        p->sig = (int)(s % 64);
        p->how = (int)((s >> 6) % 4);
        p->handler = (s & 0x80) ? ((s >> 8) & 1 ? 0 : 0x4000) : 0;
        p->flags = (int)((s >> 16) & 0x10000007u);
        p->mode = (uint32_t)((s >> 20) & 7);
        p->a = s ^ (s >> 17);
        p->b = (s >> 23) ^ (s << 7);
    }
    uint64_t sink = work_rust() ^ work_c();
    double c_sum = 0, r_sum = 0;
    /* Alternating order cancels cache/thermal ordering bias. */
    for (int r = 0; r < rounds; r++) {
        double t0, t1, t2;
        if (r % 2 == 0) {
            t0 = now_s(); sink ^= work_c(); t1 = now_s(); sink ^= work_rust(); t2 = now_s();
        } else {
            t0 = now_s(); sink ^= work_rust(); t1 = now_s(); sink ^= work_c(); t2 = now_s();
        }
        c_sum += (r % 2 == 0) ? (t1 - t0) : (t2 - t1);
        r_sum += (r % 2 == 0) ? (t2 - t1) : (t1 - t0);
    }
    printf("signal-core bench: N=%d rounds=%d c=%.1f ns/op rust=%.1f ns/op ratio=%.3f sink=%llu\n",
           N, rounds, c_sum / rounds / N * 1e9, r_sum / rounds / N * 1e9,
           (c_sum ? r_sum / c_sum : 0.0), (unsigned long long)sink);
    return 0;
}
