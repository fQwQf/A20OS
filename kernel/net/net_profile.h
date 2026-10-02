#ifndef _NET_PROFILE_H
#define _NET_PROFILE_H

/*
 * Compile-time resource tiers for the socket layer and lwIP.
 *
 * Everything here is a ceiling, not an allocation: a tier says how much the
 * stack is allowed to consume, and the pools are sized at compile time from
 * these numbers.  One profile is selected per build.
 *
 * Why this is a header rather than a set of Makefile knobs: lwIP reads its pool
 * sizes as macros from lwipopts.h before any code is generated, so the values
 * have to agree with the A20OS-side ceilings (NET_MAX_SOCKETS, NET_MAX_QUEUE)
 * at preprocessing time.  Keeping both sides in one file is what makes
 * "socket count" and "MEMP_NUM_TCP_PCB" stop drifting apart.
 *
 * The previous single hardcoded set was a server-sized configuration used for
 * every target, including the 20 KiB SRAM STM32F103 profile the README
 * advertises.  It could not fit: PBUF_POOL alone was 256 * 1536 = 384 KiB of
 * static pool, and MEMP_MEM_MALLOC was left at 0 so MEM_SIZE was unused.
 */

#ifndef CONFIG_NET_PROFILE
#define CONFIG_NET_PROFILE CONFIG_NET_PROFILE_DEFAULT
#endif

#ifndef CONFIG_NET_PROFILE_EMBEDDED
#define CONFIG_NET_PROFILE_EMBEDDED 1
#endif
#ifndef CONFIG_NET_PROFILE_DEFAULT
#define CONFIG_NET_PROFILE_DEFAULT 2
#endif
#ifndef CONFIG_NET_PROFILE_SERVER
#define CONFIG_NET_PROFILE_SERVER 3
#endif

/* Lock-contract checking.  Off by default: LWIP_ASSERT_CORE_LOCKED() expands to
 * a call and a branch at roughly fifty lwIP entry points, some of them on the
 * timer and receive paths, and that cost has not been measured.  Turn it on
 * (OPT="-DCONFIG_NET_LOCK_ASSERT=1") when investigating a lock-discipline
 * question -- it reports the owning CPU and a violation count per boot on
 * /proc/net/status as "lwip_lock: owner=<cpu> violations=<n>", and the one-shot
 * init-path violations it finds there are expected, not a defect. */
#ifndef CONFIG_NET_LOCK_ASSERT
#define CONFIG_NET_LOCK_ASSERT 0
#endif

#if CONFIG_NET_PROFILE == CONFIG_NET_PROFILE_EMBEDDED

/*
 * MCU target: one lane, no static pools.  MEMP_MEM_MALLOC=1 is the load-bearing
 * choice -- with it 0 (the derived default when MEM_SIZE is set but the macro is
 * absent) memp.c builds every pool as a static array in .bss, which is what made
 * this profile impossible.  With it 1, pools come from the lwIP heap and only
 * the live pbufs cost anything.
 *
 * The PBUF geometry trades MTU for footprint.  TCP_MSS must stay below
 * PBUF_POOL_BUFSIZE: lwIP carves the Ethernet, IP and TCP headers out of the
 * head pbuf's payload area, so a segment that fills the element leaves no room
 * for its own headers and fragments.
 */
#define NET_PROFILE_MAX_SOCKETS      8
#define NET_PROFILE_MAX_QUEUE        4

#define NET_PROFILE_MEMP_MEM_MALLOC  1
#define NET_PROFILE_MEM_SIZE         (16 * 1024)
#define NET_PROFILE_PBUF_BUFSIZE     512
#define NET_PROFILE_PBUF_POOL_SIZE   24
#define NET_PROFILE_TCP_MSS          256
#define NET_PROFILE_TCP_WND_MULT     4
#define NET_PROFILE_ARP_QUEUE        4
#define NET_PROFILE_SYS_TIMEOUT      16
#define NET_PROFILE_TCP_SEG_MULT     16
#define NET_PROFILE_TCP_PCB          8
#define NET_PROFILE_UDP_PCB          8
#define NET_PROFILE_RAW_PCB          4
#define NET_PROFILE_TCP_PCB_LISTEN   4
#define NET_PROFILE_BH_RING_SIZE     4
#define NET_PROFILE_INLINE_PAYLOAD   320
#define NET_PROFILE_SOCKET_MAX_BYTES (8 * 1024)

/*
 * Budget, stated honestly: this profile is sized so the *stack's own* pools
 * fit a small SRAM part, not so a full-featured TCP/IP stack plus an
 * application fits in 20 KiB.  With the inline staging split, one net_socket_t
 * is roughly 8 * ~700 B, so 8 sockets land near 48 KiB before any lwIP pool.
 * The MCU targets in README.md (STM32F103, 20 KiB SRAM) therefore still do not
 * fit a socket-capable lwIP, and the honest statement is that they need the
 * bring-up gates in docs/platforms/stm32f103-port.md extended to cover networking
 * before the README claim holds.  Until then this profile exists to make the
 * embedded cost *bounded and proportional to the configured ceilings* rather
 * than to claim the target boots.
 */

#elif CONFIG_NET_PROFILE == CONFIG_NET_PROFILE_SERVER

/*
 * Server target.  The ceilings that actually blocked a server were not the
 * lock but these: MEMP_NUM_SYS_TIMEOUT was 32 while MEMP_NUM_TCP_PCB was 64,
 * and every established PCB holds one sys_timeo whenever KEEPALIVE/KEEPIDLE/
 * KEEPINTVL are compiled in (all three are, in lwipopts.h).  tcp_pcb_alloc()
 * returns NULL past the sys_timeout ceiling, so the 33rd concurrent connection
 * failed to be accepted.  lwipopts.h now asserts the ordering holds.
 *
 * NET_MAX_QUEUE is per socket; a listening socket that queues 4096 accepted
 * children before the application calls accept() would be 4096 * ~1.3 KiB.
 */
#define NET_PROFILE_MAX_SOCKETS      65536
#define NET_PROFILE_MAX_QUEUE        1024

#define NET_PROFILE_MEMP_MEM_MALLOC  1
#define NET_PROFILE_MEM_SIZE         (16 * 1024 * 1024)
#define NET_PROFILE_PBUF_BUFSIZE     1536
#define NET_PROFILE_PBUF_POOL_SIZE   4096
#define NET_PROFILE_TCP_MSS          1460
#define NET_PROFILE_TCP_WND_MULT     64
#define NET_PROFILE_ARP_QUEUE        64
#define NET_PROFILE_SYS_TIMEOUT      16384
#define NET_PROFILE_TCP_SEG_MULT     256
#define NET_PROFILE_TCP_PCB          8192
#define NET_PROFILE_UDP_PCB          8192
#define NET_PROFILE_RAW_PCB          256
#define NET_PROFILE_TCP_PCB_LISTEN   256
#define NET_PROFILE_BH_RING_SIZE     16
#define NET_PROFILE_SOCKET_MAX_BYTES (32 * 1024)
#define NET_PROFILE_INLINE_PAYLOAD   1600

#else

/*
 * Default target (QEMU dev/smoke builds, physical boards with real RAM).  This
 * is the historical configuration with the two contradictions fixed: the
 * sys_timeout ceiling now tracks the PCB ceiling, and TCP_MSS matches the pool
 * element size so a full-size segment occupies exactly one element.
 */
#define NET_PROFILE_MAX_SOCKETS      1024
#define NET_PROFILE_MAX_QUEUE        128

#define NET_PROFILE_MEMP_MEM_MALLOC  1
#define NET_PROFILE_MEM_SIZE         (512 * 1024)
#define NET_PROFILE_PBUF_BUFSIZE     1536
#define NET_PROFILE_PBUF_POOL_SIZE   256
#define NET_PROFILE_TCP_MSS          1460
#define NET_PROFILE_TCP_WND_MULT     64
#define NET_PROFILE_ARP_QUEUE        32
#define NET_PROFILE_SYS_TIMEOUT      256
#define NET_PROFILE_TCP_SEG_MULT     64
#define NET_PROFILE_TCP_PCB          128
#define NET_PROFILE_UDP_PCB          64
#define NET_PROFILE_RAW_PCB          32
#define NET_PROFILE_TCP_PCB_LISTEN   32
#define NET_PROFILE_BH_RING_SIZE     16
#define NET_PROFILE_SOCKET_MAX_BYTES (32 * 1024)
#define NET_PROFILE_INLINE_PAYLOAD   1600

#endif /* CONFIG_NET_PROFILE */

/*
 * Whether the receive path may poll for packets from the idle path instead of
 * waiting for an interrupt.  On a single-lane build the historical CPU-0-only
 * poll in kernel/core/progress.c was correct, because every CPU polling one
 * global lwIP lock is a lock convoy.  Once a build has more than one lane the
 * convoy argument no longer holds, but polling is still the wrong default for a
 * device that has no queues to spread across, so it stays opt-in.
 */
#ifndef CONFIG_NET_BUSY_POLL
#define CONFIG_NET_BUSY_POLL 0
#endif

/*
 * How many packets the timer-interrupt safety-net drain may process in one
 * g_lwip_lock acquisition.  The device IRQ is the primary receive path; this
 * drain only exists so RX cannot stall if an interrupt is ever lost, so it is
 * kept short enough that it cannot dominate the critical section it runs in.
 */
#ifndef CONFIG_NET_RX_IRQ_BUDGET
#define CONFIG_NET_RX_IRQ_BUDGET 4
#endif

/*
 * Lane count.  One lane per CPU is the target shape: each lane owns a receive
 * queue, its own pbuf magazine and its own timeout wheel, and a socket's PCB
 * stays on one lane for its whole lifetime.  Until the lane plumbing lands
 * (docs/net/network-lock-contract.md tracks it) this stays 1 so the build has a
 * single netif scratch buffer and a single timeout wheel to reason about.
 */
#ifndef CONFIG_NET_LANES
#define CONFIG_NET_LANES 1
#endif

/*
 * Diagnostic amplifier for the multi-lane boot fault.  When non-zero, spins for
 * this many microseconds inside the accept drain, between dequeuing a staged pcb
 * and taking g_lwip_lock.  That gap is the only point on the accept path that
 * spans both lock domains, so widening it should turn the intermittent 4-lane
 * fault into a deterministic one and reveal which side corrupts the list.
 *
 * A measuring tool, not a fix: leave it at 0 in any real build and remove it
 * once the fault is reproducible.  See docs/net/net-lanes.md.
 */
#ifndef CONFIG_NET_RACE_DELAY_US
#define CONFIG_NET_RACE_DELAY_US 0
#endif

/*
 * PCB list sanity checker.  When enabled, TCP_REG/TCP_RMV take their checking
 * form and every insertion and removal ends in tcp_pcbs_sane(), which walks all
 * four per-lane PCB lists and asserts the bucket invariants: a pcb sits in the
 * head its lane names, catch-all pcbs live in the sentinel bucket only, and
 * list membership agrees with pcb->state.
 *
 * This is what should have caught the double-indexed removal fixed in 16304db8,
 * where tcp_pcb_remove() callers pre-subscripted by lane and TCP_RMV() then
 * subscripted again, walking bucket 2L -- out of bounds for the wildcard lane.
 * It did not catch it because TCP_DEBUG_PCB_LISTS, TCP_DEBUG, TCP_INPUT_DEBUG
 * and TCP_OUTPUT_DEBUG were all off, so tcp_pcbs_sane() was a constant 1 and
 * every one of those asserts compiled to nothing.
 *
 * Off by default because it is O(live pcbs) per insertion and removal, which
 * makes connection churn quadratic -- unaffordable for the high-concurrency
 * server path this lane work exists to serve.  Turn it on
 * (OPT="-DCONFIG_NET_PCB_SANE=1") in the multi-lane gate, where a deterministic
 * list-corruption failure is worth more than throughput.
 */
#ifndef CONFIG_NET_PCB_SANE
#define CONFIG_NET_PCB_SANE 0
#endif

#endif /* _NET_PROFILE_H */
