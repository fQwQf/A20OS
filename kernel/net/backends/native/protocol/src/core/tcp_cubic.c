/**
 * @file
 * CUBIC congestion control (RFC 8312 core) -- A20OS divergence from upstream lwIP.
 *
 * Upstream hardcodes Reno into tcp_in.c, tcp_out.c and tcp.c.  This file owns
 * the second algorithm and the call sites that consult it; see
 * kernel/external/lwip/DIVERGENCE.md section 2.5 and the header
 * lwip/priv/tcp_cubic_priv.h for what is and is not implemented.
 *
 * All arithmetic is integer fixed point.  There is deliberately not a single
 * float or double here: this is kernel code, and pulling libm's float
 * support in for four multiplies would be a poor trade.  The trade this does
 * make is that every unit has to be named and checked by hand, which is what
 * the two scales in the header are for -- window sizes in 1/256 SEGMENTS,
 * times in 1/256 SECONDS, the RFC's constants in 1/1024.
 */

#include "lwip/opt.h"
#include "lwip/priv/tcp_cubic_priv.h"

#include <string.h>

/* The name table and its inverse are OUTSIDE the LWIP_TCP_CUBIC guard on
 * purpose.  A build with CUBIC compiled out still has to answer
 * TCP_CONGESTION honestly: setsockopt("cubic") must fail with -ENOPROTOOPT
 * rather than succeed on a stack that only has Reno, and getsockopt must
 * report the one algorithm that exists.  Both need this pair of functions,
 * and neither of them touches any CUBIC state. */
const char *
tcp_cong_alg_name(u8_t alg)
{
  switch (alg) {
  case TCP_CONG_CUBIC:
    return "cubic";
  case TCP_CONG_RENO:
  default:
    /* An unrecognised value reports the algorithm that is actually running
     * rather than an empty string, so a caller that cannot parse this still
     * learns something true. */
    return "reno";
  }
}

int
tcp_cong_alg_parse(const char *name)
{
  if (name == NULL) {
    return -1;
  }
  /* strcmp, not strncmp: Linux compares the whole buffer including the NUL,
   * so "cubic\0junk" handed over with a length claiming more than the string
   * is rejected.  A prefix match here would accept a name that is not an
   * algorithm at all. */
  if (!strcmp(name, "reno")) {
    return TCP_CONG_RENO;
  }
  if (!strcmp(name, "cubic")) {
    return TCP_CONG_CUBIC;
  }
  return -1;
}

#if LWIP_TCP_CUBIC

#include "lwip/priv/tcp_priv.h"
#include "lwip/tcp.h"
#include "lwip/debug.h"

/** One tcp_ticks increment, expressed in 1/256 of a second.
 *
 *  TCP_SLOW_INTERVAL is a compile-time constant (tcp_priv.h derives it from
 *  TCP_TMR_INTERVAL), so this folds away at compile time.  It is computed
 *  rather than hardcoded because hardcoding "half a second" would be a second,
 *  silently wrong copy of a value tcp_priv.h already owns: change
 *  TCP_TMR_INTERVAL and CUBIC's time axis would then be wrong by the ratio,
 *  with no compile error. */
#define TCP_CUBIC_SEC_PER_TICK_FP \
  ((u32_t)(((u64_t)TCP_SLOW_INTERVAL * TCP_CUBIC_FP_ONE) / 1000u))

/** Largest |t - K| evaluated, in 1/256 seconds -- 256 s.  Past this the cubic
 *  term is already 0.4 * 256^3 = 6.7 M segments, which no send buffer here can
 *  use, so clamping costs nothing and keeps d^3 inside 64 bits. */
#define TCP_CUBIC_D_MAX  ((s64_t)(1 << 16))

/** Growth rounds honoured per ACK.  One round is normally consumed per cwnd
 *  acknowledged, so this only ever binds on a `acked` far larger than a window. */
#define TCP_CUBIC_MAX_ROUNDS  64

/** Integer cube root: floor(cbrt(x)).
 *
 *  Binary search with the upper bound doubled until it strictly exceeds the
 *  answer, so it cannot silently truncate for any input.  The comparison is
 *  written as m <= (x / m) / m rather than m*m*m <= x: cbrt(2^64-1) is 2642245,
 *  and that cube does not fit in a u64.  m <= floor(x/m)/m is equivalent to
 *  m^3 <= x and never overflows.  Returns 0 for x == 0, which the caller
 *  depends on -- K == 0 means the curve starts at t == 0, i.e. growth begins
 *  immediately after a timeout with no plateau. */
u64_t
tcp_cubic_icbrt64(u64_t x)
{
  u64_t lo = 0;
  u64_t hi = 1;

  if (x == 0) {
    return 0;
  }
  while (hi <= (x / hi) / hi) {
    hi <<= 1;
  }
  /* Invariant: lo^3 <= x < hi^3.  Converges to floor(cbrt(x)). */
  while (lo + 1 < hi) {
    u64_t mid = lo + (hi - lo) / 2;
    if (mid <= (x / mid) / mid) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo;
}

/** K = cbrt(W_max * (1 - beta_cubic) / C) (RFC 8312 Eq. 2), in 1/256 seconds.
 *
 *  W_max arrives in 1/256 segments and K has to leave in 1/256 seconds, which
 *  is the whole difficulty: the cube root of a quantity in segments is in
 *  seconds, so the argument is pre-scaled by 2^24 and its cube root comes back
 *  already carrying the 2^8 the caller needs.
 *
 *      arg      = W_max_seg * (1 - beta_cubic) / C
 *      arg*2^24 = (W_max_fp / 2^8) * ((1024-717)/1024) * (1024/410) * 2^24
 *              = W_max_fp * 307 * 2^16 / 410
 *      cbrt(arg * 2^24) = cbrt(arg) * 2^8 = K in 1/256 seconds
 *
 *  All of it in u64: W_max_fp alone can span the whole u32 range, and the
 *  * 2^16 puts the result that much further out again.  In u32 this overflows
 *  for any window past roughly 340 segments, and it overflows silently --
 *  wrapping K down to a small value, i.e. a connection that never grows. */
static u32_t
tcp_cubic_compute_k(u32_t w_max_fp)
{
  u64_t arg = ((u64_t)w_max_fp * (u64_t)(TCP_CUBIC_FRAC_ONE - TCP_CUBIC_BETA)
               * ((u64_t)1 << 16)) / TCP_CUBIC_C;
  return (u32_t)tcp_cubic_icbrt64(arg);
}

/** Bytes of headroom the cubic function is offering over the current cwnd,
 *  capped at one cwnd.
 *
 *  The cap is not in the RFC, and it is a bound on a pathological path rather
 *  than on ordinary growth.  tcp_ticks only advances with real time, so a
 *  normal connection cannot see the target move far in one round: the curve
 *  advances by one RTT per round and CUBIC tracks it closely.  What the cap
 *  does catch is a connection that stalls for many seconds with segments still
 *  queued -- past its RTO, where CUBIC's own curve is legitimately thousands
 *  of segments above cwnd -- and then takes a single ACK.  Without the cap one
 *  such ACK would open the window by however far the curve ran.
 *
 *  The price is real and is stated rather than hidden: cwnd can therefore
 *  overshoot W_cubic(t) by up to one doubling, because the cap is applied to
 *  the increment and not to the result.  Bounding that properly needs the
 *  round's remaining time, which this port's 500 ms clock does not have. */
static tcpwnd_size_t
tcp_cubic_headroom(const struct tcp_pcb *pcb, u32_t target_fp)
{
  /* W_cubic is in 1/256 SEGMENTS, so cwnd has to be converted before it is
   * compared against it.  cwnd << FP is 1/256 bytes -- the same bits, a
   * different unit -- and comparing that against a segment-scaled target makes
   * every window look like it is already far past the curve, which silently
   * pins the connection to the plateau and turns CUBIC back into Reno with the
   * cubic arithmetic piled on top. */
  u32_t cwnd_seg_fp = (u32_t)(((u64_t)pcb->cwnd * TCP_CUBIC_FP_ONE) / pcb->mss);
  u64_t want_bytes;

  if (target_fp <= cwnd_seg_fp) {
    return 0;
  }
  /* (target - cwnd) is in 1/256 segments and a segment is pcb->mss bytes, so
   * the whole conversion is u64: target_fp spans the whole u32 range and mss
   * pushes it past. */
  want_bytes = ((u64_t)(target_fp - cwnd_seg_fp) * (u64_t)pcb->mss) >> TCP_CUBIC_FP_BITS;
  return (tcpwnd_size_t)(want_bytes > (u64_t)pcb->cwnd ? pcb->cwnd
                                                        : (tcpwnd_size_t)want_bytes);
}

/** W_cubic(t) = C * (t - K)^3 + W_max (Eq. 1), in 1/256 segments.
 *
 *  t and K are both in 1/256 seconds, so d = t - K is too and d^3 is in
 *  1/256^3 s^3.  The denominator below undoes that cube: 1024 (C's scale) times
 *  2^16 (three 1/256 rescalings, minus the one the result keeps) turns C*d^3
 *  into 1/256 segments, which MUST be the same unit as W_max -- Eq. 1 adds the
 *  two.
 *
 *  The sign matters and is deliberately not clamped away: for t < K the cubic
 *  term is negative, and that negative term is what makes the curve start at
 *  W_cubic(0) = W_max * beta_cubic, i.e. exactly where 4.5 left cwnd.  Treating
 *  "t < K" as "no growth yet" instead would strand a connection on the plateau
 *  for a whole K, which is the largest behavioural difference between a correct
 *  CUBIC and a stub. */
static u32_t
tcp_cubic_w_cubic(const struct tcp_cubic_state *cc, u32_t t_fp)
{
  s64_t d = (s64_t)t_fp - (s64_t)cc->K;
  s64_t cube;
  s64_t v;

  if (d > TCP_CUBIC_D_MAX) {
    d = TCP_CUBIC_D_MAX;
  } else if (d < -TCP_CUBIC_D_MAX) {
    d = -TCP_CUBIC_D_MAX;
  }
  /* |d| <= 2^16, so |d^3| <= 2^48; times C <= 2^10 stays well inside s64. */
  cube = d * d * d;
  v = (s64_t)cc->W_max + (cube * TCP_CUBIC_C) / ((s64_t)TCP_CUBIC_FRAC_ONE << 16);
  if (v < 0) {
    return 0;
  }
  if (v > (s64_t)(u32_t)-1) {
    return (u32_t)-1;
  }
  return (u32_t)v;
}

/** W_est(t) = W_max*beta_cubic + alpha_aimd * (t / RTT) (RFC 8312 Eq. 4), in
 *  1/256 segments.  This is the TCP-friendly function: the window that
 *  AIMD(alpha_aimd, beta_cubic) would have reached, which is what makes CUBIC
 *  deliver "at least the same throughput as Standard TCP" on the short-RTT,
 *  small-BDP paths 4.2 exists for.
 *
 *  Unit bookkeeping, because all three terms have to land in the same place:
 *
 *      W_max * beta       W_max is 1/256 SEGMENTS, beta is 1/1024
 *                         =>  (W_max * 717) / 1024
 *      alpha * t / RTT    alpha is 1/1024 SEGMENTS PER RTT; t_fp and rtt_fp are
 *                         both 1/256 SECONDS, so the ratio is dimensionless and
 *                         the answer is in SEGMENTS -- it still has to be scaled
 *                         up to the 1/256 SEGMENTS the result is returned in
 *                         =>  (549 * t_fp * 256) / (1024 * rtt_fp)
 *
 *  That trailing * 256 is not a fudge and not optional: dropping it is a silent
 *  factor of 256 on the friendly slope, which still produces a rising curve --
 *  just one so gentle that W_est never leaves W_max*beta and the region stops
 *  existing.  Written as a multiplication rather than as a division by 4 so the
 *  scale cannot be misread: the two 1/256 denominators in t_fp/rtt_fp cancel,
 *  and what is left is exactly one 1/1024 to undo and one 256 to re-apply.
 *
 *  Overflow: 549 * 256 = 140544 and t_fp is a u32, so the numerator peaks
 *  around 6.0e14 -- comfortably inside u64.
 *
 *  RTT: the only estimator this stack has is pcb->sa, Van Jacobson's smoothed
 *  RTT, which tcp_in.c updates from `tcp_ticks - pcb->rttest` and is therefore
 *  quantised to whole TCP_SLOW_INTERVAL ticks -- 500 ms with this port's
 *  configuration.  sa == 0 means either "no sample yet" or "sub-tick RTT", which
 *  is what loopback and most LAN traffic produce, so it is floored at one tick
 *  instead of divided by.  That floor makes the slope alpha / 0.5 s = 1.07
 *  segments/s: conservative (never faster than the real RTT would allow) but not
 *  the RTT the RFC means.  Registered as a boundary in tcp_cubic_priv.h.
 */
static u32_t
tcp_cubic_w_est(const struct tcp_pcb *pcb, const struct tcp_cubic_state *cc,
                u32_t t_fp)
{
  u64_t base = ((u64_t)cc->W_max * (u64_t)TCP_CUBIC_BETA)
               / (u64_t)TCP_CUBIC_FRAC_ONE;
  s32_t sa = pcb->sa;
  if (sa < 1) {
    sa = 1;
  }
  /* sa counts TCP_SLOW_INTERVAL ticks; t_fp counts 1/256 s.  The conversion
   * constant is the same one the cubic time axis already uses, so the two
   * cannot disagree about how long a tick is. */
  u64_t rtt_fp = (u64_t)(u32_t)sa * (u64_t)TCP_CUBIC_SEC_PER_TICK_FP;
  u64_t grow = ((u64_t)TCP_CUBIC_TF_ALPHA * (u64_t)t_fp * (u64_t)TCP_CUBIC_FP_ONE)
               / ((u64_t)TCP_CUBIC_FRAC_ONE * rtt_fp);
  u64_t v = base + grow;

  if (v > (u64_t)(u32_t)-1) {
    return (u32_t)-1;
  }
  return (u32_t)v;
}

void
tcp_cubic_init(struct tcp_pcb *pcb)
{
  memset(&pcb->cubic, 0, sizeof(pcb->cubic));
}

void
tcp_cubic_on_ack(struct tcp_pcb *pcb, u32_t acked)
{
  struct tcp_cubic_state *cc = &pcb->cubic;
  u32_t now = tcp_ticks;

  if (pcb->state < ESTABLISHED) {
    return;
  }

  if (pcb->cwnd < pcb->ssthresh) {
    /* Slow start.  Unchanged from Reno: RFC 8312 4.8 keeps it, and doubling per
     * RTT is what gets a fresh connection past ssthresh quickly. */
    tcpwnd_size_t increase;
    /* Cap at 1 SMSS in the round after an RTO (RFC 3465 2.2), exactly as the
     * Reno path does, so a timeout costs the same under either algorithm. */
    u8_t num_seg = (pcb->flags & TF_RTO) ? 1 : 2;
    increase = LWIP_MIN(acked, (tcpwnd_size_t)((u32_t)num_seg * pcb->mss));
    TCP_WND_INC(pcb->cwnd, increase);
    LWIP_DEBUGF(TCP_CWND_DEBUG, ("tcp_cubic: slow start cwnd %"TCPWNDSIZE_F"\n", pcb->cwnd));
    return;
  }

  if (!cc->in_epoch) {
    /* Leaving slow start with no congestion event behind it, so RFC 8312 4.8
     * applies verbatim: K := 0 and W_max := the window congestion avoidance
     * starts from.  With K = 0 the curve is C*t^3 + W_max, so it starts exactly
     * at cwnd and rises immediately -- there is no plateau to climb out of.
     * This also covers a socket that sets a ssthresh above its initial window,
     * which lwIP permits and which would otherwise leave the pcb with no epoch
     * defined at all. */
    cc->in_epoch = 1;
    cc->epoch_start = now;
    cc->acked = 0;
    cc->W_max = (u32_t)(((u64_t)pcb->cwnd * TCP_CUBIC_FP_ONE) / pcb->mss);
    cc->K = 0;
    LWIP_DEBUGF(TCP_CWND_DEBUG, ("tcp_cubic: epoch opened from slow start\n"));
    return;
  }

  /* Application-limited flow (RFC 8312 5.8).  Both segment lists empty means
   * nothing is in flight, so the connection was waiting on the application
   * rather than on the network, and the epoch clock must not have run during
   * that wait.  Note the test is made after the caller has already appended
   * the incoming data, so "empty" really does mean "was idle when this ACK
   * arrived". */
  if (pcb->unacked == NULL && pcb->unsent == NULL) {
    cc->epoch_start = now;
    cc->acked = 0;
  }

  /* Grow once per round trip, not once per ACK.  RFC 8312 4.4 phrases the
   * increment as (W_cubic(t+RTT) - cwnd)/cwnd for each received ACK, which is
   * the same rule said per-ACK: applied once per ACK it integrates to reaching
   * W_cubic(t+RTT) by the end of the RTT.  Accumulating acknowledged bytes and
   * acting once per cwnd is how that is done in integers -- doing the per-ACK
   * division directly truncates to zero for any cwnd above a few segments, and
   * a connection whose congestion avoidance never increments is the classic
   * way to ship CUBIC that is really Reno. */
  cc->acked += acked;
  {
    /* Bounded so a single enormous `acked` -- which only a caller passing a
     * count far larger than a window could produce -- cannot turn into an
     * unbounded loop here.  The remainder stays in cc->acked, so the growth is
     * deferred rather than lost. */
    int rounds = 0;
    while (cc->acked >= (u32_t)pcb->cwnd && rounds++ < TCP_CUBIC_MAX_ROUNDS) {
      u32_t t_fp = (u32_t)((u64_t)(now - cc->epoch_start) * TCP_CUBIC_SEC_PER_TICK_FP);
      u32_t target = tcp_cubic_w_cubic(cc, t_fp);
      tcpwnd_size_t inc;

      cc->acked -= (u32_t)pcb->cwnd;
      /*
       * RFC 8312 4.2, verbatim in shape: "CUBIC checks whether W_cubic(t) is
       * less than W_est(t).  If so, CUBIC is in the TCP-friendly region and
       * cwnd SHOULD be set to W_est(t)".  So the target is the LARGER of the
       * two, not the smaller -- the friendly line is a floor that keeps a
       * short-RDP flow from being throttled below what Standard TCP would have
       * achieved, which is the whole point of the region.
       *
       * This replaces an approximation: when W_cubic offered no growth the old
       * code added Reno's rate, 1 MSS per cwnd acknowledged.  That is a rate,
       * not a target, so it neither tracked t/RTT nor responded to the flow's
       * own W_max, and on a plateau it kept a flow climbing at 1/cwnd segments
       * per RTT forever -- which for any window above one segment is slower
       * than the AIMD(alpha_aimd, beta_cubic) Eq. 4 is derived from, so the
       * "at least Standard TCP" property was not actually delivered.
       */
      {
        u32_t est = tcp_cubic_w_est(pcb, cc, t_fp);
        if (est > target) {
          target = est;
        }
      }
      inc = tcp_cubic_headroom(pcb, target);
      if (inc == 0) {
        /* Neither curve is above cwnd.  This is the plateau itself (4.5/4.6
         * deliberately create one) or a K == 0 epoch whose curve has not left
         * the window yet, and RFC 8312 says nothing about incrementing when the
         * target is below cwnd -- (W_cubic(t+RTT) - cwnd)/cwnd is simply
         * negative there.  Adding 1 MSS keeps the connection making progress
         * instead of stalling on the plateau until a loss moves the epoch; it is
         * a floor, not a growth rule, and it is reached only when 4.2's target
         * AND the cubic curve both sit at or under cwnd. */
        inc = pcb->mss;
      }
      TCP_WND_INC(pcb->cwnd, inc);
      LWIP_DEBUGF(TCP_CWND_DEBUG, ("tcp_cubic: cwnd %"TCPWNDSIZE_F"\n", pcb->cwnd));
    }
  }
}

/** Shared RFC 8312 4.5 body.  `cwnd_at_loss` is in bytes; W_max in segments.
 *
 *  `fast_convergence` is 4.6, which is applied only on the fast-retransmit
 *  path.  A timeout has already thrown the window a long way down, and letting
 *  the timeout path fast-converge too would compound two independent reductions
 *  into one. */
static void
tcp_cubic_decrease(struct tcp_pcb *pcb, tcpwnd_size_t cwnd_at_loss,
                   int fast_convergence)
{
  struct tcp_cubic_state *cc = &pcb->cubic;
  u32_t w_max_fp = (u32_t)(((u64_t)cwnd_at_loss * TCP_CUBIC_FP_ONE) / pcb->mss);

  if (fast_convergence) {
    /* Fast convergence (4.6), before the reduction.  A loss below the previous
     * event's W_max means this flow's saturation point is genuinely dropping --
     * another flow joined the network -- so W_max is cut further.  The smaller
     * W_max gives the smaller K, which pushes the plateau later and hands the
     * bandwidth over sooner.  Note this REDUCES W_max by (1 + 0.7) / 2 = 0.85:
     * despite the name, it makes the flow grow more slowly for longer, not
     * faster.  W_last_max is 0 until the first loss, which is how "no previous
     * congestion event" is spelled -- a connection that has never lost a packet
     * must not fast-converge against a loss that did not happen. */
    if (cc->last_max > 0 && w_max_fp < cc->last_max) {
      w_max_fp = (u32_t)(((u64_t)w_max_fp
                          * ((u64_t)TCP_CUBIC_FRAC_ONE + TCP_CUBIC_BETA) / 2)
                         / TCP_CUBIC_FRAC_ONE);
    }
    cc->last_max = w_max_fp;
    cc->W_max = w_max_fp;
  } else {
    cc->W_max = w_max_fp;
  }

  /* Multiplicative decrease (4.5): ssthresh = cwnd * beta_cubic from the
   * window BEFORE the reduction, floored at 2 MSS.  The u64 intermediate is
   * not optional -- a cwnd in bytes scaled by 717 overflows a u32 past about
   * 6 MB, and it overflows silently, wrapping ssthresh to a value near zero. */
  pcb->ssthresh = (tcpwnd_size_t)(((u64_t)cwnd_at_loss * TCP_CUBIC_BETA)
                                  / TCP_CUBIC_FRAC_ONE);
  if (pcb->ssthresh < (tcpwnd_size_t)(pcb->mss << 1)) {
    /* RFC 8312 4.5: ssthresh = max(ssthresh, 2).  Below 2 MSS a connection
     * cannot reliably leave slow start. */
    pcb->ssthresh = (tcpwnd_size_t)(pcb->mss << 1);
  }
  pcb->cwnd = pcb->ssthresh;

  cc->epoch_start = tcp_ticks;
  cc->acked = 0;
  cc->in_epoch = 1;
}

void
tcp_cubic_on_loss(struct tcp_pcb *pcb, tcpwnd_size_t cwnd_at_loss)
{
  tcp_cubic_decrease(pcb, cwnd_at_loss, 1);
  /* K from the possibly fast-converged W_max -- which is what makes 4.6's
   * plateau actually happen: W_cubic(0) = W_max - C*K^3 = W_max*beta_cubic,
   * and a fast-converged W_max is below cwnd_at_loss, so W_cubic(0) lands below
   * the post-loss cwnd and the curve must climb before it offers anything. */
  pcb->cubic.K = tcp_cubic_compute_k(pcb->cubic.W_max);
  LWIP_DEBUGF(TCP_CWND_DEBUG, ("tcp_cubic: loss cwnd %"TCPWNDSIZE_F" ssthresh %"TCPWNDSIZE_F
                               " W_max %u K %u\n", pcb->cwnd, pcb->ssthresh,
                               (unsigned)(pcb->cubic.W_max >> TCP_CUBIC_FP_BITS),
                               (unsigned)pcb->cubic.K >> TCP_CUBIC_FP_BITS));
}

void
tcp_cubic_on_rto(struct tcp_pcb *pcb, tcpwnd_size_t cwnd_at_rto)
{
  tcp_cubic_decrease(pcb, cwnd_at_rto, 0);
  /* RFC 8312 4.7: the first congestion avoidance after a timeout uses Eq. 1
   * with K set to 0 and W_max set to the window at the start of that avoidance
   * -- here, the post-reduction cwnd.  The curve then leaves from where the
   * timeout left the window instead of walking back down to the pre-timeout
   * W_max first, which is the whole reason 4.7 is a separate rule from 4.5. */
  pcb->cubic.K = 0;
  pcb->cubic.W_max = (u32_t)(((u64_t)pcb->cwnd * TCP_CUBIC_FP_ONE) / pcb->mss);
  LWIP_DEBUGF(TCP_CWND_DEBUG, ("tcp_cubic: rto cwnd %"TCPWNDSIZE_F" W_max %u K 0\n",
                               pcb->cwnd,
                               (unsigned)(pcb->cubic.W_max >> TCP_CUBIC_FP_BITS)));
}

#endif /* LWIP_TCP_CUBIC */