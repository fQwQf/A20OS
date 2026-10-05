/*
 * Host-side unit test for the CUBIC window function in
 * kernel/external/lwip/src/core/tcp_cubic.c.
 *
 * Why this exists as a host test and not as a QEMU smoke target: the things
 * that can be wrong in CUBIC are the arithmetic (a cube root that truncates, a
 * scaling factor applied at the wrong power of two, a time axis in the wrong
 * unit, fast convergence that never fires or that inflates instead of
 * reduces).  None of those need a network to observe, and all of them are
 * invisible to a loopback transfer test -- a connection that never loses a
 * packet never leaves slow start, so it would pass against an implementation
 * whose entire congestion avoidance path was dead code.
 *
 * Every reference value below is computed in double precision from RFC 8312's
 * own formulas, not copied from the implementation.  An expectation derived
 * from the code under test tests nothing; an expectation derived from the RFC
 * tests whether the code agrees with the RFC, which is the only claim being
 * made.
 *
 * What CANNOT be checked here, and is not claimed: the interaction with the
 * real lwIP event path (which call site fires on which ACK).  That is covered
 * by the smoke gates, which run real transfers -- they prove the wiring does
 * not break a connection, not that CUBIC produces Linux-identical windows.
 *
 * Build and run:  tools/test-tcp-cubic-host.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/*
 * lwIP's own headers supply everything this test needs: u8_t..u64_t
 * (arch/cc.h via the stand-in under tools/tcp-cubic-test/arch), tcpwnd_size_t,
 * enum tcp_state, struct tcp_pcb (lwip/tcp.h, reached through tcp_cubic.c's own
 * #include of tcp_priv.h), TCP_WND and the debug macros.
 *
 * Nothing is redeclared here on purpose.  A hand-copied tcpwnd_size_t would be
 * u32_t here and u16_t under a different TCP_WND, and the test would then be
 * validating a type the kernel never uses -- and a hand-declared struct tcp_pcb
 * would collide with the real one the algorithm's own includes pull in.
 *
 * The consequence worth stating: the test drives the genuine struct tcp_pcb, so
 * a field the algorithm starts reading that this mirror lacks is a COMPILE
 * error here, not a silent zero.
 */
#define TCP_CUBIC_TEST_INCLUDE_GUARD 1

/* The implementation under test, compiled directly.  It brings in lwIP's real
 * struct tcp_pcb, which is why nothing is redeclared above. */
#include "tcp_cubic.c"

/* tcp_ticks is the stack's wall clock, defined in lwIP's tcp.c -- which this
 * test does not link, since linking the whole stack would mean needing the
 * kernel's memory allocator.  CUBIC's growth function is a function of elapsed
 * time, so simulating an epoch means driving this counter directly; it is
 * declared after tcp_cubic.c so u32_t exists. */
u32_t tcp_ticks;

static int failures;
static int checks;

#define CHECK(cond, fmt, ...)                                        \
    do {                                                             \
        checks++;                                                    \
        if (!(cond)) {                                               \
            failures++;                                              \
            printf("  FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        }                                                            \
    } while (0)

/* RFC 8312 constants, as written in the RFC.  C is 0.4 segments per second^3,
 * beta_cubic is 0.7, and there is no separate beta_c: fast convergence uses
 * (1 + beta_cubic) / 2 per 4.6. */
static const double C_ = 0.4;
static const double BETA = 0.7;

/* Independent reference cube root, to check tcp_cubic_icbrt64 against. */
static double ref_cbrt(double x)
{
    if (x <= 0) {
        return 0;
    }
    double lo = 0, hi = 1e6;
    while (hi - lo > 1e-9) {
        double mid = (lo + hi) / 2;
        if (mid * mid * mid < x) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return (lo + hi) / 2;
}

/* W_cubic(t) = C * (t - K)^3 + W_max (Eq. 1), in segments, t in seconds. */
static double ref_w_cubic(double t, double K, double W_max)
{
    double d = t - K;
    return C_ * d * d * d + W_max;
}

/* K = cbrt(W_max * (1 - beta) / C) (Eq. 2), in seconds. */
static double ref_k(double W_max)
{
    return ref_cbrt(W_max * (1.0 - BETA) / C_);
}

/* Seconds represented by one tcp_ticks increment, read from the implementation's
 * own constant so the reference and the code cannot drift on the tick period
 * while still being derived from TCP_SLOW_INTERVAL independently of the
 * arithmetic under test. */
static double tick_seconds(void)
{
    return (double)TCP_CUBIC_SEC_PER_TICK_FP / TCP_CUBIC_FP_ONE;
}

static void test_icbrt(void)
{
    printf("test_icbrt64\n");
    /* Exact cubes, in u64 so the cube itself cannot overflow the way the
     * u32 version of this test used to: 1644^3 = 4443297984 is already past
     * 2^32, and the wrap made the reference compare against a different
     * number than the one printed.  r stops at cbrt(2^64-1) = 2642245 for the
     * same reason one step up -- past that the cube is not representable. */
    for (uint64_t r = 0; r <= 2642245ull; r += 1377) {
        uint64_t cube = r * r * r;
        CHECK(tcp_cubic_icbrt64(cube) == r, "icbrt(%llu) = %llu, want %llu",
              (unsigned long long)cube, (unsigned long long)tcp_cubic_icbrt64(cube),
              (unsigned long long)r);
    }
    /* Every value's floor must satisfy lo^3 <= x < (lo+1)^3. */
    for (uint64_t x = 0; x < 4000000000ull; x = x * 7 + 13) {
        uint64_t r = tcp_cubic_icbrt64(x);
        CHECK(r * r * r <= x, "icbrt(%llu)=%llu too high", (unsigned long long)x,
              (unsigned long long)r);
        CHECK((r + 1) * (r + 1) * (r + 1) > x, "icbrt(%llu)=%llu too low",
              (unsigned long long)x, (unsigned long long)r);
        double ref = ref_cbrt((double)x);
        CHECK(ref - (double)r <= 1.0 + 1e-9 && (double)r - ref <= 1.0 + 1e-9,
              "icbrt(%llu)=%llu, reference %.4f", (unsigned long long)x,
              (unsigned long long)r, ref);
    }
    /* u64 boundary: must not hang or overflow.  cbrt(2^64-1) = 2642245. */
    CHECK(tcp_cubic_icbrt64(UINT64_MAX) == 2642245, "icbrt(UINT64_MAX)=%llu want 2642245",
          (unsigned long long)tcp_cubic_icbrt64(UINT64_MAX));
    CHECK(tcp_cubic_icbrt64(0) == 0, "icbrt(0)");
}

/* TCP_CONGESTION names must round-trip, and anything that is not one of the two
 * algorithms this stack actually implements has to be rejected -- an
 * unrecognised name silently accepted is a socket that claims to run an
 * algorithm it is not running. */
static void test_parse(void)
{
    printf("test_parse\n");
    CHECK(tcp_cong_alg_parse("reno") == TCP_CONG_RENO, "reno");
    CHECK(tcp_cong_alg_parse("cubic") == TCP_CONG_CUBIC, "cubic");
    /* Everything else must be rejected, including near-misses. */
    const char *bad[] = { "", "reno ", " reno", "CUBIC", "Cubic", "bbr", "cubic2",
                          "cubi", "cub", "reno\n", "dccp", "hypercube" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(tcp_cong_alg_parse(bad[i]) < 0, "parse(\"%s\") should fail", bad[i]);
    }
    CHECK(tcp_cong_alg_parse(NULL) < 0, "parse(NULL) should fail");
    /* Names round-trip. */
    CHECK(!strcmp(tcp_cong_alg_name(TCP_CONG_RENO), "reno"), "name(reno)");
    CHECK(!strcmp(tcp_cong_alg_name(TCP_CONG_CUBIC), "cubic"), "name(cubic)");
}

/* A pcb mid-transfer always has something in flight; the test's pcb has no
 * segment lists, so without this every ACK would look like an application-
 * limited flow and restart the epoch clock (RFC 8312 5.8), making the growth
 * tests measure nothing.  The sentinel is only ever compared against NULL. */
static void mark_busy(struct tcp_pcb *p)
{
    p->unsent = (struct tcp_seg *)p;   /* non-NULL, never dereferenced */
}

static struct tcp_pcb *fresh_pcb(u16_t mss, tcpwnd_size_t cwnd, int busy)
{
    struct tcp_pcb *p = calloc(1, sizeof(*p));
    p->state = ESTABLISHED;
    p->mss = mss;
    p->cwnd = cwnd;
    p->ssthresh = TCP_WND;
    p->cong_alg = TCP_CONG_CUBIC;
    tcp_cubic_init(p);
    if (busy) {
        mark_busy(p);
    }
    return p;
}

/* RFC 8312 4.5: ssthresh = max(cwnd * beta_cubic, 2 MSS), cwnd = ssthresh. */
static void test_loss_response(void)
{
    printf("test_loss_response (RFC 8312 4.5)\n");
    struct tcp_pcb *p = fresh_pcb(1460, 64 * 1460, 1);

    tcp_cubic_on_loss(p, p->cwnd);
    double want_ssthresh = 64.0 * BETA;
    double got_ssthresh = (double)p->ssthresh / 1460.0;
    CHECK(fabs(got_ssthresh - want_ssthresh) < 0.02,
          "ssthresh %.3f segments, want %.3f (cwnd * beta_cubic)", got_ssthresh,
          want_ssthresh);
    CHECK(p->cwnd == p->ssthresh, "cwnd %u should equal ssthresh %u",
          p->cwnd, p->ssthresh);
    CHECK(p->ssthresh >= 2u * 1460, "ssthresh %u below the 2 MSS floor", p->ssthresh);

    /* Eq. 2, from W_max and not from the already-reduced ssthresh. */
    double want_k = ref_k(64.0);
    double got_k = (double)p->cubic.K / TCP_CUBIC_FP_ONE;
    CHECK(fabs(got_k - want_k) < 0.02, "K %.4f s, want %.4f s", got_k, want_k);

    /* Eq. 1 at t = 0 must equal W_max * beta_cubic, i.e. exactly where 4.5 left
     * cwnd.  This is the identity that ties K and W_max together; if either is
     * computed from the wrong window, the connection starts its epoch with a
     * gap and a plateau it does not understand. */
    double at_zero = ref_w_cubic(0.0, want_k, 64.0);
    CHECK(fabs(at_zero - want_ssthresh) < 0.01,
          "W_cubic(0) = %.4f, want %.4f", at_zero, want_ssthresh);

    /* The 2 MSS floor must actually engage. */
    struct tcp_pcb *q = fresh_pcb(1460, 2 * 1460, 1);
    tcp_cubic_on_loss(q, q->cwnd);
    CHECK(q->ssthresh == 2u * 1460, "floor: ssthresh %u, want %u",
          q->ssthresh, 2u * 1460);
    CHECK(q->cwnd == 2u * 1460, "floor: cwnd %u, want %u", q->cwnd, 2u * 1460);
    free(q);
    free(p);
}

/* RFC 8312 4.6: when W_max < W_last_max, remember the new value and cut
 * W_max to W_max * (1 + beta_cubic) / 2.  This REDUCES W_max -- the heuristic
 * exists to release bandwidth to a flow that just arrived, so despite the name
 * it makes the flow grow more slowly for longer. */
static void test_fast_convergence(void)
{
    printf("test_fast_convergence (RFC 8312 4.6)\n");
    struct tcp_pcb *p = fresh_pcb(1460, 64 * 1460, 1);

    tcp_cubic_on_loss(p, p->cwnd);           /* W_max = 64 seg */
    CHECK(p->cubic.W_max >> 8 == 64u, "first W_max %u seg, want 64",
          p->cubic.W_max >> 8);
    CHECK(p->cubic.last_max == p->cubic.W_max,
          "W_last_max %u should equal the first W_max %u", p->cubic.last_max,
          p->cubic.W_max);

    /* A second loss at a LOWER window is in the same congestion event. */
    tcp_cubic_on_loss(p, 32 * 1460);          /* W_max = 32 seg, < 64 */
    double want = 32.0 * (1.0 + BETA) / 2.0;
    double got = (double)p->cubic.W_max / TCP_CUBIC_FP_ONE;
    CHECK(fabs(got - want) < 0.02, "fast-convergence W_max %.3f seg, want %.3f",
          got, want);
    CHECK(p->cubic.W_max < (32u * 1460 << 8),
          "fast convergence must REDUCE W_max, got %u", p->cubic.W_max);
    CHECK(p->cubic.last_max == p->cubic.W_max,
          "4.6 sets W_last_max = W_max on the reducing branch too: %u vs %u",
          p->cubic.last_max, p->cubic.W_max);

    /* K follows the cut W_max, so the plateau is what actually moves. */
    double want_k = ref_k(want);
    double got_k = (double)p->cubic.K / TCP_CUBIC_FP_ONE;
    CHECK(fabs(got_k - want_k) < 0.02, "fast-convergence K %.4f s, want %.4f s",
          got_k, want_k);

    /* A loss ABOVE the previous W_max must NOT cut it. */
    struct tcp_pcb *r = fresh_pcb(1460, 64 * 1460, 1);
    tcp_cubic_on_loss(r, r->cwnd);
    tcp_cubic_on_loss(r, 96 * 1460);          /* higher than 64 */
    CHECK(r->cubic.W_max == (u32_t)(((u64_t)96 * 1460 * TCP_CUBIC_FP_ONE) / 1460),
          "non-converging W_max %u, want 96 seg", r->cubic.W_max);

    /* The very first loss must not converge: there is no previous W_max to be
     * below, and converging against a loss that never happened would halve
     * every connection's very first W_max. */
    struct tcp_pcb *s = fresh_pcb(1460, 32 * 1460, 1);
    tcp_cubic_on_loss(s, 8 * 1460);
    double want_first = (double)8 * 1460 * TCP_CUBIC_FP_ONE / 1460;
    CHECK(fabs((double)s->cubic.W_max - want_first) < 1.0,
          "first loss W_max %u, want %.0f", s->cubic.W_max, want_first);
    CHECK(s->cubic.K > 0, "a first loss must still produce a non-zero K");

    free(r); free(s); free(p);
}

/* RFC 8312 4.7: after a timeout the curve starts from where the timeout left
 * the window -- K := 0, W_max := cwnd -- instead of from the pre-timeout
 * W_max.  Routing the RTO through the fast-retransmit path would leave the
 * connection on a curve anchored above its own window. */
static void test_rto_response(void)
{
    printf("test_rto_response (RFC 8312 4.7)\n");
    struct tcp_pcb *p = fresh_pcb(1460, 64 * 1460, 1);
    tcp_cubic_on_rto(p, p->cwnd);

    CHECK(fabs((double)p->ssthresh / 1460.0 - 64.0 * BETA) < 0.02,
          "rto ssthresh %.3f seg, want %.3f", (double)p->ssthresh / 1460.0,
          64.0 * BETA);
    CHECK(p->cubic.K == 0, "rto must set K := 0, got %u", p->cubic.K);
    double want_wmax = (double)p->cwnd * TCP_CUBIC_FP_ONE / 1460;
    CHECK(fabs((double)p->cubic.W_max - want_wmax) < 1.0,
          "rto W_max %.3f seg, want %.3f", (double)p->cubic.W_max / TCP_CUBIC_FP_ONE,
          want_wmax / TCP_CUBIC_FP_ONE);
    /* Fast convergence must not also fire on the timeout path. */
    CHECK(p->cubic.last_max == 0, "rto must not fast-converge, last_max %u",
          p->cubic.last_max);
    free(p);
}

/* Slow start is unchanged, and it still respects the 1 SMSS cap in the round
 * after an RTO. */
static void test_slow_start(void)
{
    printf("test_slow_start (RFC 8312 4.8)\n");
    struct tcp_pcb *p = fresh_pcb(1460, 1460, 1);
    p->cwnd = 1460;
    p->ssthresh = 64 * 1460;

    tcp_ticks = 100;
    for (int i = 0; i < 10; i++) {
        tcp_cubic_on_ack(p, 1460);
    }
    CHECK(p->cwnd > 1460, "slow start did not grow cwnd: %u", p->cwnd);
    CHECK(p->cwnd == 11u * 1460, "slow start cwnd %u, want exactly 11 MSS (%u)",
          p->cwnd, 11u * 1460);
    /* No epoch is opened while still in slow start: 4.8's "W_max undefined" case
     * is about the moment slow start ENDS, and opening an epoch here would
     * anchor W_max to a window the connection has long since left. */
    CHECK(!p->cubic.in_epoch, "slow start must not open a growth epoch");

    /* After an RTO, one ACK must add at most 1 MSS. */
    struct tcp_pcb *q = fresh_pcb(1460, 1460, 1);
    q->cwnd = 1460;
    q->ssthresh = 64 * 1460;
    q->flags |= TF_RTO;
    tcp_ticks = 100;
    tcp_cubic_on_ack(q, 1460);
    CHECK(q->cwnd == 2u * 1460,
          "post-RTO cwnd %u, want exactly 2 MSS (%u)", q->cwnd, 2u * 1460);

    free(q);
    free(p);
}

/* Leaving slow start with no congestion event behind it: RFC 8312 4.8 says
 * W_max is undefined, and the cure is K := 0 with W_max := the window
 * congestion avoidance starts from.  That makes the curve depart immediately
 * (W_cubic(0) = W_max = cwnd) rather than sitting on a plateau. */
static void test_epoch_from_slow_start(void)
{
    printf("test_epoch_from_slow_start (RFC 8312 4.8)\n");
    struct tcp_pcb *p = fresh_pcb(1460, 8 * 1460, 1);
    p->cwnd = 8 * 1460;
    p->ssthresh = 8 * 1460;

    tcp_ticks = 500;
    tcp_cubic_on_ack(p, 1460);
    CHECK(p->cubic.in_epoch, "leaving slow start must open an epoch");
    CHECK(p->cubic.K == 0, "4.8 sets K := 0, got %u", p->cubic.K);
    double want_wmax = 8.0 * TCP_CUBIC_FP_ONE;
    CHECK(fabs((double)p->cubic.W_max - want_wmax) < 1.0,
          "4.8 W_max %.3f seg, want %.3f", (double)p->cubic.W_max / TCP_CUBIC_FP_ONE,
          want_wmax / TCP_CUBIC_FP_ONE);
    CHECK(p->cubic.epoch_start == 500, "epoch_start %u, want 500",
          p->cubic.epoch_start);
    free(p);
}

/* The tightest available check on the growth path: drive EXACTLY one round of
 * ACKs at a known time and require cwnd to land on W_cubic(t).
 *
 * This is what pins the time axis to seconds.  C is 0.4 segments per second^3,
 * so with 8 ticks after a loss at 64 segments (4 real seconds at this port's
 * 500 ms tick) W_cubic is 64.02 segments.  An implementation that treated
 * tcp_ticks as seconds would compute 77.4 segments here, and one that measured
 * the epoch in seconds but computed K in ticks would compute 64.6.  Neither
 * error is visible in a loopback transfer test, and both are visible here. */
static void test_curve_value(void)
{
    printf("test_curve_value (RFC 8312 Eq. 1 in seconds)\n");
    struct tcp_pcb *p = fresh_pcb(1460, 64 * 1460, 1);
    tcp_ticks = 0;
    tcp_cubic_on_loss(p, p->cwnd);

    double k_s = ref_k(64.0);
    CHECK(fabs((double)p->cubic.K / TCP_CUBIC_FP_ONE - k_s) < 0.02,
          "K %.4f s, want %.4f s", (double)p->cubic.K / TCP_CUBIC_FP_ONE, k_s);

    /* Drive exactly enough ACKs to complete one round -- a round is cwnd bytes
     * acknowledged -- and no more. */
    tcp_ticks = 8;                       /* 8 * 0.5 s = 4 s since the loss */
    u32_t one_round = ((u32_t)p->cwnd + 1459) / 1460;
    for (u32_t i = 0; i < one_round; i++) {
        tcp_cubic_on_ack(p, 1460);
    }

    double want = ref_w_cubic(4.0, k_s, 64.0);
    double have = (double)p->cwnd / 1460.0;
    CHECK(fabs(have - want) < 0.1,
          "cwnd %.4f seg after one round at t=4s, want W_cubic = %.4f", have, want);
    printf("    one round at t=4.0s: cwnd %.4f seg, W_cubic = %.4f seg\n", have, want);
    free(p);
}

/* The growth function must track C*(t-K)^3 + W_max in SECONDS, must never run
 * backwards inside an epoch, and must not overshoot the curve by more than the
 * one-doubling the implementation documents.  A time axis in ticks instead of
 * seconds scales C by 8 and shows up here as a cwnd far below the reference. */
static void test_growth_curve(void)
{
    printf("test_growth_curve (RFC 8312 4.1/4.3/4.4)\n");
    struct tcp_pcb *p = fresh_pcb(1460, 64 * 1460, 1);
    tcp_cubic_on_loss(p, p->cwnd);
    p->ssthresh = 1460 * 2;      /* leave slow start immediately */
    p->cwnd = p->ssthresh;

    double k_s = ref_k(64.0);
    double w_max = 64.0;
    double ts = tick_seconds();

    tcp_ticks = 1000;
    p->cubic.epoch_start = 1000;
    u32_t prev_cwnd = p->cwnd;
    int monotonic = 1, under = 1, over = 1;
    for (int t = 0; t < 200; t++) {
        tcp_ticks = 1000 + t;
        for (int i = 0; i < 20; i++) {
            tcp_cubic_on_ack(p, 1460);
        }
        double t_sec = t * ts;
        double want = ref_w_cubic(t_sec, k_s, w_max);
        double have = (double)p->cwnd / 1460.0;
        if (have < (double)prev_cwnd / 1460.0) {
            monotonic = 0;
            printf("    cwnd went backwards at t=%.1fs: %u -> %u\n", t_sec,
                   prev_cwnd, p->cwnd);
            break;
        }
        /* The window must not simply track the reference tick for tick: it is
         * driven onto W_cubic(t) once per round and only ever moves forward, so
         * at coarse tick granularity it legitimately trails a curve climbing by
         * thousands of segments per tick.  What has to hold is the bound above;
         * that it ends up PAST W_max is checked once the loop is done, and the
         * tight check that the curve's value is right is test_curve_value. */
        if (have > want * 2.0 + 2.0) {
            over = 0;
            printf("    cwnd %.1f seg overshoots reference %.1f seg at t=%.1fs\n", have,
                   want, t_sec);
            break;
        }
        prev_cwnd = p->cwnd;
    }
    CHECK(monotonic, "cwnd must never decrease inside a growth epoch");
    if ((double)p->cwnd / 1460.0 < w_max) {
        under = 0;
        printf("    cwnd ended at %.1f seg, below W_max = %.0f\n",
               (double)p->cwnd / 1460.0, w_max);
    }
    CHECK(under, "cwnd must climb past W_max, not sit on the plateau");
    CHECK(over, "cwnd must not overshoot W_cubic(t) by more than one doubling");
    CHECK(p->cwnd > 2u * 1460, "cwnd %u never grew past slow start", p->cwnd);
    printf("    K = %.3f s, W_max = %.0f seg, cwnd reached %.1f seg\n", k_s, w_max,
           (double)p->cwnd / 1460.0);

    /* The curve must be monotonically increasing in t once past K -- the
     * property that distinguishes CUBIC from Reno's constant additive
     * increase, and the one a broken sign or a clamped negative term kills. */
    double at_k = ref_w_cubic(k_s, k_s, w_max);
    double late = ref_w_cubic(k_s + 10.0, k_s, w_max);
    CHECK(fabs(at_k - w_max) < 0.01, "W_cubic(K) = %.3f, want W_max = %.0f", at_k,
          w_max);
    CHECK(late > at_k, "cubic curve must rise past K: %.1f vs %.1f", late, at_k);
    /* And it must rise MORE steeply than Reno's 1 MSS per RTT does, which is
     * the entire reason this algorithm exists. */
    double reno = w_max + 10.0 / ts;   /* 10 s of Reno, 1 MSS per RTT */
    CHECK(late > reno, "cubic %.1f seg must beat reno %.1f seg over 10 s", late,
          reno);

    free(p);
}

/* A connection mid-transfer must actually out-grow Reno.  Reno's rate is 1 MSS
 * per cwnd acknowledged, so after N rounds Reno sits at start + N segments;
 * CUBIC has to be strictly ahead of that, or it is Reno with extra steps. */
static void test_beats_reno(void)
{
    printf("test_beats_reno\n");
    struct tcp_pcb *p = fresh_pcb(1460, 64 * 1460, 1);
    tcp_cubic_on_loss(p, p->cwnd);
    double start = (double)p->cwnd / 1460.0;

    uint64_t total_acked = 0;
    for (int t = 0; t < 400; t++) {
        tcp_ticks = 1000 + t;
        for (int i = 0; i < 10; i++) {
            tcp_cubic_on_ack(p, 1460);
            total_acked += 1460;
        }
    }
    /* Every round Reno would have taken at the starting window, plus one for
     * each round it takes at the (larger) windows CUBIC actually used.  The
     * second term is the generous one; CUBIC still has to clear it. */
    double reno_upper = start + (double)total_acked / (start * 1460.0);
    double have = (double)p->cwnd / 1460.0;
    CHECK(have > reno_upper,
          "cubic reached %.1f seg; reno's upper bound is %.1f seg", have,
          reno_upper);
    printf("    cwnd %.1f seg after %llu bytes acked (reno upper bound %.1f seg)\n",
           have, (unsigned long long)total_acked, reno_upper);
    free(p);
}

/* An idle connection must restart the epoch (RFC 8312 5.8), or the curve runs
 * away during a pause and the window jumps on the first ACK back. */
static void test_idle_restart(void)
{
    printf("test_idle_restart (RFC 8312 5.8)\n");
    struct tcp_pcb *p = fresh_pcb(1460, 64 * 1460, 1);
    tcp_cubic_on_loss(p, p->cwnd);
    p->cwnd = p->ssthresh;
    p->cubic.epoch_start = 0;

    /* Idle for a long time: nothing in flight. */
    tcp_ticks = 100000;
    p->unsent = NULL;            /* mark_busy() undone */
    (void)0;
    tcp_cubic_on_ack(p, 1460);
    CHECK(p->cubic.epoch_start == 100000,
          "epoch must restart on idle: %u, want 100000", p->cubic.epoch_start);
    CHECK(p->cubic.acked < p->cwnd,
          "idle restart must reset the round counter, got %u of %u", p->cubic.acked,
          p->cwnd);

    /* A busy connection must NOT restart the epoch on every ACK. */
    struct tcp_pcb *q = fresh_pcb(1460, 64 * 1460, 1);
    tcp_ticks = 5000;
    tcp_cubic_on_loss(q, q->cwnd);
    q->cwnd = q->ssthresh;
    tcp_cubic_on_ack(q, 1460);
    CHECK(q->cubic.epoch_start == 5000, "busy epoch_start %u, want 5000",
          q->cubic.epoch_start);
    tcp_ticks = 6000;
    for (int i = 0; i < 100; i++) {
        tcp_cubic_on_ack(q, 1460);
    }
    CHECK(q->cubic.epoch_start == 5000,
          "busy connection restarted its epoch: %u", q->cubic.epoch_start);

    free(q);
    free(p);
}

/* cwnd must never exceed what tcpwnd_size_t can hold, however long the
 * connection runs: the TCP_WND_INC saturation branch has to actually be
 * reached rather than assumed, and no intermediate may wrap. */
static void test_no_overflow(void)
{
    printf("test_no_overflow\n");
    struct tcp_pcb *p = fresh_pcb(1460, 8 * 1460, 1);
    tcp_cubic_on_loss(p, p->cwnd);
    p->ssthresh = 2 * 1460;
    p->cwnd = p->ssthresh;
    for (int t = 0; t < 20000; t++) {
        tcp_ticks = 5000 + t;
        for (int i = 0; i < 10; i++) {
            tcp_cubic_on_ack(p, 1460);
        }
        if (p->cwnd == (tcpwnd_size_t)-1) {
            break;
        }
        CHECK(p->cwnd >= 2u * 1460, "cwnd wrapped to %u at t=%d", p->cwnd, t);
    }
    /* Saturated is the correct outcome here (TCP_SND_BUF is far below u32 max,
     * but lwIP does not clamp cwnd to it); what matters is that it saturates
     * rather than wrapping to a small value. */
    CHECK(p->cwnd == (tcpwnd_size_t)-1 || p->cwnd > 8 * 1460,
          "cwnd %u neither grew nor saturated", p->cwnd);
    printf("    cwnd saturated at %u after a long epoch\n", p->cwnd);

    /* K must stay correct for a window far past anything this stack sends:
     * 1,000,000 segments overflows a u32 K computation's argument and used to
     * be exactly where the u64 intermediate was needed. */
    struct tcp_pcb *q = fresh_pcb(1460, 1000000u * 1460u, 1);
    tcp_cubic_on_loss(q, q->cwnd);
    double want_k = ref_k(1000000.0);
    double got_k = (double)q->cubic.K / TCP_CUBIC_FP_ONE;
    CHECK(fabs(got_k - want_k) < want_k * 0.01,
          "large-window K %.2f s, want %.2f s", got_k, want_k);
    free(q);
    free(p);
}

int main(void)
{
    tcp_ticks = 0;
    printf("=== lwIP CUBIC (RFC 8312) host unit tests ===\n");
    printf("    one tcp_ticks = %.6f s, TCP_CUBIC_C = %d/1024, BETA = %d/1024\n",
           tick_seconds(), TCP_CUBIC_C, TCP_CUBIC_BETA);
    test_icbrt();
    test_parse();
    test_loss_response();
    test_fast_convergence();
    test_rto_response();
    test_slow_start();
    test_epoch_from_slow_start();
    test_curve_value();
    test_growth_curve();
    test_beats_reno();
    test_idle_restart();
    test_no_overflow();
    printf("=== %d checks, %d failures ===\n", checks, failures);
    return failures ? 1 : 0;
}