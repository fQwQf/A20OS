/**
 * @file
 * CUBIC congestion control (RFC 8312) -- A20OS divergence from upstream lwIP.
 *
 * Upstream lwIP 2.2.2d has exactly one congestion control algorithm, Reno,
 * hardcoded into tcp_in.c / tcp_out.c / tcp.c.  This header and tcp_cubic.c
 * factor the window update out so a second algorithm can be selected per
 * connection; see kernel/external/lwip/DIVERGENCE.md section 2.5.
 *
 * What is implemented is the RFC 8312 core, not a full Linux port.  Section
 * numbers are RFC 8312 unless stated otherwise:
 *   - 4.1, 4.3, 4.4  the growth function W_cubic(t) = C * (t - K)^3 + W_max,
 *                     with cwnd driven onto the candidate target once per RTT
 *   - Eq. 2           K = cbrt(W_max * (1 - beta_cubic) / C)
 *   - 4.5             multiplicative decrease: W_max = cwnd, then
 *                     ssthresh = max(cwnd * beta_cubic, 2 MSS), cwnd = ssthresh
 *   - 4.6             fast convergence, when W_max < W_last_max:
 *                     W_last_max = W_max; W_max = W_max * (1 + beta_cubic) / 2
 *   - 4.7             after a timeout, K := 0 and W_max := cwnd at the start of
 *                     the following congestion avoidance
 *   - 4.8             the same (K := 0, W_max := cwnd) when slow start ends
 *                     without a loss and W_max would otherwise be undefined
 *   - 5.8             application-limited flows: t excludes the idle period, so
 *                     the epoch clock restarts rather than running through it
 *
 * What is deliberately NOT implemented, and must not be assumed:
 *   - 4.2, the TCP-friendly region, is approximated rather than implemented.
 *     Eq. 4's W_est(t) needs an RTT-driven term; what stands in for it is
 *     Reno's own rate (1 MSS per cwnd acknowledged) applied whenever W_cubic
 *     offers no growth.  That errs in the safe direction -- the connection
 *     never grows slower than Standard TCP -- but it is not W_est, and it does
 *     not deliver the "at least AIMD(0.529, 0.7) throughput" guarantee.
 *   - TCP-AQ / HyStart / DCTCP / Prague
 *   - the ECN and RTT-variance couplings Linux layers on top of CUBIC
 *   - persistence of W_max beyond the life of one tcp_pcb
 *
 * Time base.  W_cubic's C is 0.4 segments per second^3, so Eq. 1's time axis is
 * SECONDS.  The stack's only clock is tcp_ticks, one tick per tcp_slowtmr(),
 * i.e. TCP_SLOW_INTERVAL milliseconds (500 by default), so K and the elapsed
 * epoch are converted into 1/256 of a second before they meet the cubic term.
 * Getting that conversion wrong does not merely lose accuracy: it scales the
 * growth rate by 8, which is the difference between CUBIC and a cubic function
 * running three times too slow.  The tick granularity itself only costs
 * accuracy -- CUBIC's growth function is smooth, and 500 ms is well under one
 * RTT on the links this stack drives.
 */

#ifndef LWIP_PRIV_TCP_CUBIC_PRIV_H
#define LWIP_PRIV_TCP_CUBIC_PRIV_H

#include "lwip/opt.h"

/** Congestion control algorithms selectable per connection.
 *
 *  Outside the LWIP_TCP_CUBIC guard on purpose: the socket layer names TCP_CONG_RENO
 *  even on a build where CUBIC is compiled out, in order to accept "reno" and
 *  reject "cubic" as -ENOPROTOOPT rather than as an undeclared identifier.  The
 *  value 0 is also what a memset-to-zero pcb carries, so it is the default. */
#define TCP_CONG_RENO   0
#define TCP_CONG_CUBIC  1

/** Name of a congestion control algorithm, for TCP_CONGESTION.
 *
 *  Outside the guard with the values above, because a build without CUBIC still
 *  has to report its one real algorithm -- and report "reno" rather than an
 *  empty string for a value it does not recognise, so a caller that cannot
 *  parse the result still learns something true. */
const char *tcp_cong_alg_name(u8_t alg);

/** Parse a TCP_CONGESTION name.  Returns -1 for an unknown name, which the
 *  socket layer turns into -ENOPROTOOPT rather than silently accepting it. */
int tcp_cong_alg_parse(const char *name);

#if LWIP_TCP_CUBIC

/* tcpbase.h defines tcpwnd_size_t and pulls in nothing that includes tcp.h, so
 * unlike lwip/tcp.h it is safe to include from here.  Deliberately does NOT
 * include lwip/tcp.h: tcp.h includes this header to get struct tcp_cubic_state
 * for struct tcp_pcb, so including it back would be circular.
 *
 * struct tcp_pcb is only ever a pointer in the prototypes below, so it is
 * forward-declared rather than defined.  A prototype naming an undeclared
 * struct would declare it inside the parameter list, where it is invisible to
 * every other translation unit -- and socket_control.c passes a real
 * struct tcp_pcb *, so the two would be different types. */
#include "lwip/tcpbase.h"

struct tcp_pcb;

/** Two fixed-point scales are in use, for two different jobs.
 *
 *  FRAC (1/1024) holds the RFC's dimensionless constants -- C and beta_cubic --
 *  which are multiplied against window sizes and against each other.  1024 is
 *  enough that 0.4 and 0.7 are not visibly wrong after truncation
 *  (410/1024 = 0.4004, 717/1024 = 0.7002).
 *
 *  FP (1/256) holds window sizes in SEGMENTS and times in SECONDS.  Both
 *  scales being 1/256 is not a coincidence: W_cubic adds a window to C*d^3, so
 *  the two sides must share a scale or the sum is meaningless.
 *
 *  A window in 1/256 segments covers 16.7 M segments, far past anything
 *  TCP_SND_BUF allows here.  The cube of a time needs 64-bit intermediates in
 *  tcp_cubic.c, which is where it is done. */
#define TCP_CUBIC_FRAC_BITS   10
#define TCP_CUBIC_FRAC_ONE    (1 << TCP_CUBIC_FRAC_BITS)
#define TCP_CUBIC_FP_BITS     8
#define TCP_CUBIC_FP_ONE      (1 << TCP_CUBIC_FP_BITS)

/** RFC 8312 constants in 1/1024 units.  Exact-ish, and exact enough: the
 *  truncation error is under 0.05% on each of them.
 *
 *  There is no beta_c (0.85) constant here.  Fast convergence uses
 *  (1 + beta_cubic) / 2 per RFC 8312 4.6 -- the RFC's own pseudocode, not the
 *  0.85 that Linux's fast-convergence comment inherited from an earlier
 *  revision.  Keeping a separate BETA_C constant is what let that drift go
 *  unnoticed, so it does not exist. */
#define TCP_CUBIC_C           410   /* 0.4   */
#define TCP_CUBIC_BETA        717   /* 0.7   */

/** Per-PCB CUBIC state.  Window fields are in 1/256 segments, K is in 1/256
 *  seconds, epoch_start is in raw ticks, `acked` is in bytes. */
struct tcp_cubic_state {
  /** W_max: the constant term of Eq. 1, i.e. the window at the last loss,
   *  possibly reduced by fast convergence.  1/256 segments. */
  u32_t W_max;
  /** last_max: W_max as of the previous congestion event, for 4.6's
   *  "did this loss land below the last one?" test.  Zero until the first loss,
   *  because a connection that has never lost a packet has no W_last_max to be
   *  below -- seeding this with the initial window would make the very first
   *  loss fast-converge against a loss that did not happen.  1/256 segments. */
  u32_t last_max;
  /** K: the cube-root term, cbrt(W_max * (1 - beta) / C), in 1/256 SECONDS. */
  u32_t K;
  /** epoch_start: tick at which the current growth epoch began. */
  u32_t epoch_start;
  /** acked: bytes acknowledged inside the current window-growth step, carried
   *  between ACKs so growth happens every cwnd bytes rather than per ACK. */
  u32_t acked;
  /** in_epoch: set once a growth epoch is under way.  Distinguishes "still in
   *  slow start, never had a loss" -- which 4.8 says has no usable W_max -- from
   *  "epoch finished, sitting on the plateau of the cubic curve". */
  u8_t in_epoch;
};

/** Integer cube root: floor(cbrt(x)).  Binary search, so it cannot truncate,
 *  and u64-bounded so K can be computed for windows past 32 segments without
 *  the intermediate argument overflowing.  A u32-only version of this forces
 *  the caller to rescale the argument down, which loses K's low bits. */
u64_t tcp_cubic_icbrt64(u64_t x);

/** Initialise a pcb's CUBIC state for a fresh connection: a cleared epoch and
 *  no W_max at all.
 *
 *  W_max is deliberately left undefined rather than seeded with the initial
 *  window.  RFC 8312 4.8 says exactly this -- a connection that leaves slow
 *  start without a loss has no W_max, and enters congestion avoidance with
 *  K := 0 and W_max := the window it actually reached.  Seeding W_max with the
 *  initial window instead would silently produce a curve anchored to a value
 *  the connection passed through a long time ago. */
void tcp_cubic_init(struct tcp_pcb *pcb);

/** Congestion window update for bytes newly acknowledged.
 *  Replaces the Reno block in tcp_receive(); called only once the ACK has been
 *  classified as acknowledging new data and the pcb is ESTABLISHED or beyond,
 *  which is exactly the precondition the Reno block is guarded by. */
void tcp_cubic_on_ack(struct tcp_pcb *pcb, u32_t acked);

/** Loss response for a fast retransmit (RFC 8312 4.5 + 4.6).
 *  `cwnd_at_loss` is the window BEFORE the reduction, because both lwIP call
 *  sites have already modified pcb->cwnd by the time they could call. */
void tcp_cubic_on_loss(struct tcp_pcb *pcb, tcpwnd_size_t cwnd_at_loss);

/** Loss response for a retransmission timeout (RFC 8312 4.7).
 *  Same multiplicative decrease as 4.5, but the following congestion
 *  avoidance starts with K := 0 and W_max := cwnd, so the curve departs
 *  immediately from wherever the timeout left the window instead of walking
 *  back down to W_max first. */
void tcp_cubic_on_rto(struct tcp_pcb *pcb, tcpwnd_size_t cwnd_at_rto);

#endif /* LWIP_TCP_CUBIC */
#endif /* LWIP_PRIV_TCP_CUBIC_PRIV_H */
