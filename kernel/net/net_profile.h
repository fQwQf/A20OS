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
 * init-path violations it finds there are expected, not a defect.
 *
 * The same switch now drives the net-lock probe as well
 * (kernel/net/net_lock_probe.c), which is what closes the gap the lwIP half
 * never had: the two net-lock rules (address order in net_sock_lock2(), and
 * "never a bucket lock under a socket lock") were prose only.  Values:
 *
 *   0  no probe at all.  /proc/net/status says "net_lock: not checked", so an
 *      absent probe is never read as a clean one.  This is the default and it
 *      cannot abort anything.
 *   1  probe on; a violation records its site and panics.  A counting signal
 *      gets trained to be ignored, so a violation has to stop the machine.
 *   2  probe on; a violation is counted and its site recorded, no panic.  For a
 *      long soak where the violation count at the end is worth more than a dead
 *      machine on the first hit.
 */
#ifndef CONFIG_NET_LOCK_ASSERT
#define CONFIG_NET_LOCK_ASSERT 0
#endif

/* Reference-count checking.  The net_socket_t refcount ledger
 * (socket_internal.h, net_socket_free()) is always compiled in and always
 * counted -- allocs, frees, live, faults all render on /proc/net/status -- and
 * this switch only decides what a *fault* does.  Off by default, because a
 * fault means some path dropped one reference too many and the machine is
 * already in undefined behaviour by the time the counter moves; that is a
 * reason to stop, not a reason to keep running and print a number.
 * OPT="-DCONFIG_NET_REF_ASSERT=1" turns the fault into a panic. */
#ifndef CONFIG_NET_REF_ASSERT
#define CONFIG_NET_REF_ASSERT 0
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
#define NET_PROFILE_PBUF_POOL_SIZE   10
#define NET_PROFILE_TCP_MSS          256
#define NET_PROFILE_TCP_WND_MULT     4
#define NET_PROFILE_ARP_QUEUE        4
#define NET_PROFILE_SYS_TIMEOUT      16
#define NET_PROFILE_TCP_SEG_MULT     8
#define NET_PROFILE_TCP_PCB          8
#define NET_PROFILE_UDP_PCB          8
#define NET_PROFILE_RAW_PCB          4
#define NET_PROFILE_TCP_PCB_LISTEN   4
#define NET_PROFILE_REASSDATA        8
#define NET_PROFILE_FRAG_PBUF        8

/*
 * Receive staging, which is what made eight sockets impossible here.
 *
 * A net_socket_t embeds its own bottom-half ring: NET_BH_RING_SIZE staged
 * inbound events, each carrying NET_BH_INLINE_PAYLOAD bytes of payload plus a
 * ~208 B header, plus one spill pointer per slot.  That is the dominant term in
 * the struct and the only term a profile can shrink without removing a feature,
 * which is why these two numbers are per-tier.  Measured on riscv64 LP64:
 *
 *   ring 4 / payload 320 -> event 528 B, ring 2152 B, net_socket_t 3640 B,
 *                           8 sockets 29120 B = 1.39x a 20 KiB part
 *   ring 2 / payload 256 -> event 464 B, ring  952 B, net_socket_t 2440 B,
 *                           8 sockets 19520 B = 0.93x a 20 KiB part
 *
 * Payload 256 is the floor, not a round number: _Static_assert() in
 * socket_inet.c requires NET_BH_INLINE_PAYLOAD >= TCP_MSS, and this tier's MSS
 * is 256.  Below that every TCP segment would take the spill path and stage by
 * pbuf reference, which is correct but costs a refcount per segment and turns
 * the copy into a chain walk.
 *
 * Ring depth 2 is safe rather than merely small: net_inet_tcp_stage_payload()
 * reserves capacity for the whole segment up front and returns false when the
 * ring is full, which makes the lwIP callback answer ERR_MEM and lwIP parks the
 * pbuf in refused_data for a later retry.  A full ring backpressures, it does
 * not lose the segment.  What depth 2 costs is burst absorption -- see
 * docs/server-readiness.md, "嵌入式档能力边界".
 */
#define NET_PROFILE_BH_RING_SIZE     2
#define NET_PROFILE_INLINE_PAYLOAD   256

/*
 * Per-socket ceiling, tightened from 8 KiB to 2.5 KiB to match the ring above:
 * the old bound was three times the struct it was guarding, so it could not
 * fail even if the staging regressed to a server-sized ring.  8 x 2560 is
 * exactly the 20 KiB the part has, which is what makes the socket-table assert
 * below the thing that actually pins this tier.
 */
#define NET_PROFILE_SOCKET_MAX_BYTES 2560

/*
 * Per-term budget for the whole socket table: NET_PROFILE_MAX_SOCKETS x
 * sizeof(net_socket_t).  The struct's real size is not visible here (this
 * header is included from socket_internal.h *before* the struct is defined),
 * so the assert that uses this number lives in socket_internal.h and is
 * checked against a real sizeof().  This macro is the ceiling that sizeof is
 * measured against.
 */
#define NET_PROFILE_SOCKET_BUDGET    (20 * 1024)

/* SACK and TCP timestamps off: both grow the TCP header of every data segment
 * and every established PCB, and on a 512 B pool element that comes straight out
 * of the payload budget.  A profile whose stated goal is a bounded footprint
 * should not spend it on options that only pay off on a real network path. */
#define NET_PROFILE_TCP_SACK_OUT     0
#define NET_PROFILE_TCP_MAX_SACK_NUM 1
#define NET_PROFILE_TCP_TIMESTAMPS   0
#define NET_PROFILE_TCP_CUBIC        0

/*
 * Conntrack + NAT ceilings.  The table is unconditional .bss, so these are real
 * bytes: NET_PROFILE_CONNTRACK_ENTRY_BYTES x entries, where the entry-size
 * ceiling is asserted against the real sizeof() in netfilter_nat.c.
 * Halved from the 64 the tier was launched with.  A conntrack entry is tracked
 * only when netfilter is loaded, and this tier's honest answer to "how many
 * simultaneous flows will it track" is a small one -- see docs/server-readiness.md
 * for what the reduction costs.  The table is paid for on every boot whether or
 * not netfilter ever runs, so its ceiling belongs in the tier's memory budget
 * rather than in a feature nobody enabled.
 */
#define NET_PROFILE_CONNTRACK_ENTRIES 32
#define NET_PROFILE_CONNTRACK_BUCKETS 8

/*
 * The whole of this tier's networking RAM, in one number, as the sum of four
 * terms that are each asserted somewhere real:
 *
 *   socket table   NET_PROFILE_MAX_SOCKETS x sizeof(net_socket_t)
 *                  -- assert in socket_internal.h (real sizeof), 19520 B
 *   frame arrays   NET_PROFILE_PACKET_RING_SLOTS x slot + netif states
 *                  -- macro bound here, real sizeof in socket_packet.c and
 *                     lwip_stack.c, 3732 B measured
 *   filter tables  conntrack + NAT rule tables
 *                  -- assert in netfilter_nat.c (real sizeof), 3072 B
 *   lwIP heap      NET_PROFILE_MEM_SIZE
 *                  -- a static array in memp/mem.c, 16384 B
 *
 * 42644 B measured.  The ceiling is 44 KiB, which leaves roughly 2.4 KiB of
 * slack for the structs this file cannot see (the obj_cache descriptors, the
 * per-lane and per-bucket bookkeeping, and any growth in the three terms
 * above).
 *
 * This replaces the old framing, and the change is a real capability statement
 * rather than bookkeeping: the tier was previously described as a 20 KiB
 * profile whose largest single term overran 20 KiB.  It is now described as a
 * profile that needs about 42 KiB and is asserted not to need more than 44 KiB.
 * 20 KiB was never achievable for a socket-capable lwIP -- the eight sockets
 * alone are 19520 B -- and the way that used to be expressed was a ceiling that
 * the code violated.  docs/server-readiness.md carries the numbers.
 */
#define NET_PROFILE_FILTER_BUDGET    (32 * 64 + 1280)
#define NET_PROFILE_TOTAL_BUDGET     (44 * 1024)

/*
 * What this tier's pool ceilings add up to against its own heap.
 *
 * With MEMP_MEM_MALLOC=1 the pools are not static arrays -- memp.c's
 * do_memp_malloc_pool() is mem_malloc(desc->size), so a pool's ceiling is a
 * claim on MEM_SIZE and not an independent reservation.  That makes the two
 * numbers directly comparable, and it makes the sum below a real question: a
 * set of ceilings that adds up to more than the heap is a set of ceilings that
 * cannot all be reached, and MEMP_STATS reports the shortfall as err > 0 on
 * whichever pool loses the race.
 *
 * Measured on riscv64 LP64 (14 pools compile in), the element sizes at this
 * tier's geometry: PBUF_POOL 536, TCP_PCB 296, TCP_PCB_LISTEN 104,
 * UDP_PCB 96, RAW_PCB 96, ND6_QUEUE 88, REASSDATA 40, FRAG_PBUF 40,
 * MLD6_GROUP 32, TCP_SEG 32, ARP_QUEUE 24, PBUF 24, SYS_TIMEOUT 16.  Each is
 * rounded up to the next multiple of 8 below, which is the direction that can
 * only make this assert fire early.
 */
#define NET_PROFILE_MEMP_PBUF_POOL_ELEM      544
#define NET_PROFILE_MEMP_PBUF_ELEM           32
#define NET_PROFILE_MEMP_TCP_PCB_ELEM        304
#define NET_PROFILE_MEMP_TCP_PCB_LISTEN_ELEM 112
#define NET_PROFILE_MEMP_UDP_PCB_ELEM        104
#define NET_PROFILE_MEMP_RAW_PCB_ELEM        104
#define NET_PROFILE_MEMP_TCP_SEG_ELEM        40
#define NET_PROFILE_MEMP_REASSDATA_ELEM      48
#define NET_PROFILE_MEMP_FRAG_PBUF_ELEM      48
#define NET_PROFILE_MEMP_ARP_QUEUE_ELEM      32
#define NET_PROFILE_MEMP_ND6_QUEUE_ELEM      96
#define NET_PROFILE_MEMP_MLD6_GROUP_ELEM     40
#define NET_PROFILE_MEMP_SYS_TIMEOUT_ELEM    24

/* lwIP's own opt.h defaults for the two IPv6 multicast/neighbour pools are 20
 * and 4.  Both are per-part permanent heap claims on a tier whose heap is four
 * times smaller than the default tier's, and neither is reachable without an
 * IPv6 multicast group join or a neighbour solicitation, so they are scaled
 * here rather than left at a default that was chosen for a different part. */
#define NET_PROFILE_ND6_QUEUE         6
#define NET_PROFILE_MLD6_GROUP        2

#define NET_PROFILE_MEMP_CLAIM_BYTES \
    (NET_PROFILE_PBUF_POOL_SIZE * NET_PROFILE_MEMP_PBUF_POOL_ELEM + \
     (NET_PROFILE_PBUF_POOL_SIZE / 2) * NET_PROFILE_MEMP_PBUF_ELEM + \
     NET_PROFILE_TCP_PCB * NET_PROFILE_MEMP_TCP_PCB_ELEM + \
     NET_PROFILE_TCP_PCB_LISTEN * NET_PROFILE_MEMP_TCP_PCB_LISTEN_ELEM + \
     NET_PROFILE_UDP_PCB * NET_PROFILE_MEMP_UDP_PCB_ELEM + \
     NET_PROFILE_RAW_PCB * NET_PROFILE_MEMP_RAW_PCB_ELEM + \
     (NET_PROFILE_TCP_SEG_MULT * NET_PROFILE_TCP_WND_MULT) * \
         NET_PROFILE_MEMP_TCP_SEG_ELEM + \
     NET_PROFILE_REASSDATA * 2 * NET_PROFILE_MEMP_REASSDATA_ELEM + \
     NET_PROFILE_FRAG_PBUF * NET_PROFILE_MEMP_FRAG_PBUF_ELEM + \
     NET_PROFILE_ARP_QUEUE * NET_PROFILE_MEMP_ARP_QUEUE_ELEM + \
     NET_PROFILE_ND6_QUEUE * NET_PROFILE_MEMP_ND6_QUEUE_ELEM + \
     NET_PROFILE_MLD6_GROUP * NET_PROFILE_MEMP_MLD6_GROUP_ELEM + \
     NET_PROFILE_SYS_TIMEOUT * NET_PROFILE_MEMP_SYS_TIMEOUT_ELEM)

/*
 * The reconciliation this file's own comment used to say was needed: the sum of
 * this tier's pool ceilings has to fit the heap those same pools draw from.
 *
 * Before this change the sum was 22880 B against a 16384 B heap, so the tier
 * declared ceilings it could never reach -- which under MEMP_MEM_MALLOC=1 is
 * not a harmless overstatement, because memp has no per-pool cap: the pools
 * simply compete, and whichever loses reports err > 0 on /proc/a20/netmem as an
 * unexplained receive drop.  Now 13332 B of claims against 16384 B, with 3052 B
 * left for everything memp does not serve (pbuf_custom chains, netconn, DNS
 * tables and lwIP's own allocations).
 */
_Static_assert(NET_PROFILE_MEMP_CLAIM_BYTES <= NET_PROFILE_MEM_SIZE,
               "this profile's pool ceilings add up to more than its own "
               "MEM_SIZE; with MEMP_MEM_MALLOC=1 every pool draws from that one "
               "heap, so the tier is declaring capacity it cannot allocate and "
               "the shortfall will surface as memp err > 0, not as a clean "
               "allocation failure");

/*
 * A bottom-half ring of one slot is not a small ring, it is a broken one: the
 * producer and the consumer would have to interleave perfectly for two adjacent
 * segments to both be staged, and the second would sit in lwIP's refused_data
 * until the first was drained.  Two is the floor that still absorbs a segment
 * arriving while another is being read.  The per-tier depth is chosen above;
 * this only refuses the degenerate one.
 */
_Static_assert(NET_PROFILE_BH_RING_SIZE >= 2,
               "a bottom-half ring of one slot backpressures every second "
               "received segment onto lwIP's refused_data path");

/*
 * Frame-buffer geometry, which is the other half of the pbuf story.
 *
 * The device scratch buffers were a hardcoded 1536 on every profile while
 * PBUF_POOL_BUFSIZE was 512 here, so a full-size frame was received into a
 * 1536 B buffer and then handed to pbuf_alloc() as one 1536 B request: three
 * chained 512 B elements for every frame.  PBUF_POOL_SIZE=24 therefore held
 * eight full-size frames instead of twenty-four, and the profile's own pool
 * count silently stopped meaning what it says.
 *
 * A buffer is only worth sizing to the pool when the link MTU lets a frame fit
 * one element, so the MTU follows the buffer rather than staying at 1500: the
 * frame is the Ethernet header plus the MTU, and MTU + ETH_HLEN must be <=
 * both the scratch buffer and the pool element.  ETH_HLEN is not visible here
 * (lwip/ethernet.h is downstream of this header), so the 14 is spelled out and
 * the exact sizeof() side of the same relation is asserted in lwip_stack.c.
 */
#define NET_PROFILE_PACKET_RING_SLOTS 4
#define NET_PROFILE_PACKET_FRAME_SIZE NET_PROFILE_PBUF_BUFSIZE
#define NET_PROFILE_NETIF_MAX_DEVS    1
#define NET_PROFILE_NETIF_FRAME_SIZE  NET_PROFILE_PBUF_BUFSIZE
#define NET_PROFILE_NETIF_MTU         (NET_PROFILE_PBUF_BUFSIZE - 14)
#define NET_PROFILE_STATIC_BUDGET     (20 * 1024)

/*
 * What this tier costs, stated as one number instead of as an aspiration.
 *
 * NET_PROFILE_STATIC_BUDGET above is the *frame-array* term only and is
 * deliberately loose; it has never been the size of this profile.  The size is
 * NET_PROFILE_TOTAL_BUDGET, asserted as a sum in socket_internal.h.  A 20 KiB
 * part cannot hold this profile: eight sockets alone are 19520 B, and a
 * socket-capable lwIP does not fit in the 1280 B that would be left.  The
 * ceiling is therefore 44 KiB, which is what the code now guarantees rather
 * than the 20 KiB it used to name and violate.
 *
 * The other half of that statement is unchanged by this work and still worth
 * repeating: the MCU targets in README.md (STM32F103, 20 KiB SRAM) do not build
 * a network stack at all -- PROFILE=mcu compiles a curated source list
 * (components/trim.toml [profile.mcu].sources) that contains neither the
 * kernel/net sources nor lwIP, so NET_PROFILE has no effect on that target.
 * This profile targets a part with roughly 64 KiB of RAM, not a 20 KiB one.
 * See README.md and docs/server-readiness.md.
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
#define NET_PROFILE_PBUF_BUFSIZE     1600
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

/* SACK and timestamps on.  lwIP implements both and leaves them at opt.h's 0,
 * so nothing here negotiated them: against a real peer that means Reno-only
 * loss recovery and no PAWS.  PBUF_POOL_BUFSIZE is 1600 rather than 1536
 * because the options share the head element with the payload --
 * 1460 + 54 + 12 (TS) + 36 (four SACK blocks) = 1562 -- and the assertion in
 * lwipopts.h checks that sum rather than the old option-free 54. */
#define NET_PROFILE_TCP_SACK_OUT     1
#define NET_PROFILE_TCP_MAX_SACK_NUM 4
#define NET_PROFILE_TCP_TIMESTAMPS   1
#define NET_PROFILE_TCP_CUBIC        1

/* Unchanged from the historical hardcoded 16 x 1536 / 4 x (1536 + 1536): this
 * profile is what those constants were, and the accounting below simply states
 * where the 37312 B went instead of leaving it implicit.  The netif scratch
 * buffer stays at 1536 even with options on: it holds whole frames
 * (MTU 1500 + 14 = 1514), while the option-bearing *segment* is what has to
 * fit a pbuf element, and that is the 1600 above. */
#define NET_PROFILE_PACKET_RING_SLOTS 16
#define NET_PROFILE_PACKET_FRAME_SIZE 1536
#define NET_PROFILE_NETIF_MAX_DEVS    4
#define NET_PROFILE_NETIF_FRAME_SIZE  1536
#define NET_PROFILE_NETIF_MTU         1500
#define NET_PROFILE_STATIC_BUDGET     (128 * 1024)

/*
 * A conntrack entry is 64 bytes, so 1024 entries is 64 KiB of static table.
 * Kept well under the PCB ceilings above it: a tracked flow is coarser than a
 * PCB, and a table that fills at a thousand flows evicts by LRU rather than
 * refusing.
 */
#define NET_PROFILE_CONNTRACK_ENTRIES 1024
#define NET_PROFILE_CONNTRACK_BUCKETS 128

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
#define NET_PROFILE_PBUF_BUFSIZE     1600
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

/* Same reasoning as the SERVER tier: options on, pool element grown to match. */
#define NET_PROFILE_TCP_SACK_OUT     1
#define NET_PROFILE_TCP_MAX_SACK_NUM 4
#define NET_PROFILE_TCP_TIMESTAMPS   1
#define NET_PROFILE_TCP_CUBIC        1

/* Unchanged from the historical hardcoded values; only now stated.  See the
 * SERVER block above for what these cost. */
#define NET_PROFILE_PACKET_RING_SLOTS 16
#define NET_PROFILE_PACKET_FRAME_SIZE 1536
#define NET_PROFILE_NETIF_MAX_DEVS    4
#define NET_PROFILE_NETIF_FRAME_SIZE  1536
#define NET_PROFILE_NETIF_MTU         1500
#define NET_PROFILE_STATIC_BUDGET     (64 * 1024)

/*
 * QEMU dev/smoke default.  A conntrack entry is 64 bytes, so 256 entries is
 * 16 KiB of static table -- affordable here, and enough that a NAT gate never
 * reaches the eviction path by accident.
 */
#define NET_PROFILE_CONNTRACK_ENTRIES 256
#define NET_PROFILE_CONNTRACK_BUCKETS 32

#endif /* CONFIG_NET_PROFILE */

/*
 * Geometry the profile above has to satisfy, checked on all three rungs
 * rather than discovered on an MCU at run time.
 *
 * NET_PROFILE_NETIF_STATE_OVERHEAD is a deliberate *upper* bound on the
 * non-frame part of one netif state (idx + device_t* + ops pointer + caps +
 * eleven u64 counters + the link_pending byte, measured 128 B on riscv64 LP64
 * and smaller on ILP32).  Being generous in that direction is the safe
 * direction for a budget assert; the exact sizeof() is pinned where the struct
 * exists, in lwip_stack.c.
 *
 * 128, not 96: this term has moved twice, and both times it was a real struct
 * growth that the assert caught rather than a rounding fudge.  link_pending
 * (the RTNLGRP_LINK broadcast's deferred-publish flag) pushed the tail padding
 * out of the struct and took it 96 -> 104; then the driver work added `caps`
 * and the tx_sg_frames/tx_sg_bytes pair, taking it 104 -> 128.  The value is
 * not a number to be tuned for headroom -- lwip_stack.c asserts sizeof()
 * against it, so anything below the true figure fails the riscv64 build rather
 * than silently overstating the budget.
 */
/*
 * Pools that lwIP's opt.h sizes with its own defaults rather than through the
 * profile.  They are spelled here so a tier can scale them, and the defaults
 * are opt.h's values so the default and server rungs keep exactly the ceilings
 * they had before any of this was profile-scoped.
 */
#ifndef NET_PROFILE_CONNTRACK_ENTRY_BYTES
#define NET_PROFILE_CONNTRACK_ENTRY_BYTES 64
#endif
#ifndef NET_PROFILE_REASSDATA
#define NET_PROFILE_REASSDATA 16
#endif
#ifndef NET_PROFILE_FRAG_PBUF
#define NET_PROFILE_FRAG_PBUF 32
#endif
#ifndef NET_PROFILE_ND6_QUEUE
#define NET_PROFILE_ND6_QUEUE 20
#endif
#ifndef NET_PROFILE_MLD6_GROUP
#define NET_PROFILE_MLD6_GROUP 4
#endif

#define NET_PROFILE_PACKET_SLOT_BYTES (4 + NET_PROFILE_PACKET_FRAME_SIZE)
#ifndef NET_PROFILE_NETIF_STATE_OVERHEAD
#define NET_PROFILE_NETIF_STATE_OVERHEAD 128
#endif
#define NET_PROFILE_NETIF_STATE_BYTES \
    (2 * NET_PROFILE_NETIF_FRAME_SIZE + NET_PROFILE_NETIF_STATE_OVERHEAD)

/* A max-size frame is one Ethernet header plus the MTU, and it has to fit both
 * the device scratch buffer and a single pbuf-pool element -- otherwise the
 * receive path silently chains elements and PBUF_POOL_SIZE stops meaning what
 * the profile says it means. */
_Static_assert(NET_PROFILE_NETIF_MTU + 14 <= NET_PROFILE_NETIF_FRAME_SIZE,
               "a full-MTU frame does not fit the device RX/TX scratch buffer; "
               "the receive path would truncate it or fail the transmit");
_Static_assert(NET_PROFILE_NETIF_FRAME_SIZE <= NET_PROFILE_PBUF_BUFSIZE,
               "the device scratch buffer is larger than a pbuf-pool element, "
               "so a full-MTU frame chains pool elements instead of using one");
_Static_assert(NET_PROFILE_NETIF_MTU + 14 <= NET_PROFILE_PBUF_BUFSIZE,
               "a full-MTU frame does not fit one pbuf-pool element");
/* The AF_PACKET capture ring copies what the driver hands it, so its slot
 * cannot be larger than the buffer the frame was read into. */
_Static_assert(NET_PROFILE_PACKET_FRAME_SIZE <= NET_PROFILE_NETIF_FRAME_SIZE,
               "the AF_PACKET capture ring's slot is larger than the device "
               "scratch buffer it copies from");
/*
 * The total this profile costs in unconditional .bss.  These two arrays were
 * the reason "embedded" could not mean small: they were compiled outside the
 * profile's scope at 37312 B on every rung, which is 1.8x a 20 KiB part on its
 * own.  docs/server-readiness.md records the measurement; this assert is what
 * keeps a future hardcoded constant from quietly reinstating it.
 */
_Static_assert(NET_PROFILE_PACKET_RING_SLOTS * NET_PROFILE_PACKET_SLOT_BYTES +
                   NET_PROFILE_NETIF_MAX_DEVS * NET_PROFILE_NETIF_STATE_BYTES
               <= NET_PROFILE_STATIC_BUDGET,
               "the profile-scope frame buffers exceed the profile's static "
               "budget; these arrays are unconditional .bss that no runtime "
               "counter reports, so the ceiling has to be a compile-time one");

/*
 * Stage D: frames staged per lane between the device interrupt and the point
 * that runs the protocol stack on them.
 *
 * The device ring is drained by copying each frame into one of these, and the
 * copy is the only per-packet work left in interrupt context: no pbuf, no
 * ARP/TCP/UDP, no bottom-half.  Everything else moved out, which is the whole
 * point of the stage -- per-packet protocol processing is the dominant cost and
 * it is what wants to run on the owning lane instead of on whichever CPU took
 * the interrupt.
 *
 * Depth is the honest form of the backpressure decision.  A lane's queue
 * holding frames means those frames are already off the device, so a full
 * queue cannot be backpressured onto the wire; the producer drops and counts
 * instead, and TCP retransmits.  Four is enough to absorb a short burst while
 * a lane is being claimed, and small enough that four lanes do not cost more
 * static memory than the whole rest of this profile's frame budget -- which is
 * why it gets a quarter of the budget rather than a share of the remainder.
 *
 * NOTHING IS ALLOCATED AT ONE LANE.  The array lives inside
 * `#if CONFIG_NET_LANES > 1` in net_lane.h, so an embedded build does not pay
 * for it and, more importantly, its preprocessed source does not contain it.
 *
 * The budget is half of what the rest of this profile's frame arrays get, which
 * is why four lanes on the default rung fit and more than about a dozen would
 * not.  An embedded rung with more than one lane is EXPECTED TO FAIL THE ASSERT
 * IN net_lane.c, and that is the correct outcome rather than a bug: four lanes
 * of per-lane frame state does not fit a 20 KiB part, and the honest response
 * to that configuration is a build error saying so, not a silent overrun of the
 * budget the rest of this file exists to enforce.
 */
#ifndef NET_PROFILE_LANE_RXQ_SLOTS
#define NET_PROFILE_LANE_RXQ_SLOTS 4
#endif
#ifndef NET_PROFILE_LANE_RXQ_BUDGET
#define NET_PROFILE_LANE_RXQ_BUDGET (NET_PROFILE_STATIC_BUDGET / 2)
#endif
_Static_assert(NET_PROFILE_LANE_RXQ_SLOTS >= 2,
               "a lane receive queue of one slot turns every frame into a "
               "drop unless the consumer happens to be looking at that lane");
_Static_assert(NET_PROFILE_NETIF_MTU + 14 <= NET_PROFILE_NETIF_FRAME_SIZE,
               "a full-MTU frame does not fit a lane receive slot, so the "
               "device drain would have to truncate it");

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
 * What the per-lane memp tables cost, so the tiers can be reconciled against it.
 *
 * Stage C2 gave every pool a per-lane descriptor row plus a per-(lane, pool)
 * pair of counters (kernel/external/lwip/src/core/memp.c).  Three static tables
 * follow, and only lane 0's descriptor row is free: it aliases memp_pools[]
 * rather than copying it, so the descriptor table is (N-1) rows, not N.
 *
 *   descriptors  (N-1) * MEMP_MAX * sizeof(struct memp_desc)
 *   counters        N  * MEMP_MAX * sizeof(struct memp_lane_count)
 *   pool index      N  * MEMP_MAX * sizeof(struct memp_desc *)
 *
 * The four sizes below are the LWIP side's layout, restated here because this
 * is the header the tier budgets are written in and lwip/memp.h cannot see it
 * in time to size anything.  They are not allowed to drift: memp.c asserts this
 * expression against the real sizeof of the tables it declares, so changing one
 * side without the other is a build failure, not a silently wrong budget.
 * Measured on riscv64 LP64: 14 pools compile in at all three tiers,
 * sizeof(struct memp_desc) is 24 (desc + stats + u16 size, padded to the
 * pointer alignment), and sizeof(struct memp_lane_count) is 8 (two u32).
 *
 * Why this is the whole memory cost of per-lane-izing memp, and it is worth
 * being explicit because the opposite is the natural guess: it is not.  All
 * three tiers set MEMP_MEM_MALLOC=1, under which a descriptor holds a size and
 * nothing else and do_memp_malloc_pool() is a mem_malloc() of that size against
 * the one shared heap.  A per-lane descriptor therefore draws from the same heap
 * as its siblings, so NET_PROFILE_MEMP_CLAIM_BYTES and MEM_SIZE are unchanged
 * by lane count -- there is no xN on pool memory here.  What a future
 * !MEMP_MEM_MALLOC build would cost instead is N copies of every pool's base
 * array, which is the xN this file would then have to budget, and which is why
 * that switch is a profile decision rather than a detail.
 *
 * Cost on riscv64 LP64: 0 B at one lane, 1904 B at four, identical on all
 * three tiers because it does not scale with any tier's heap.
 */
#define NET_PROFILE_MEMP_POOLS        14
#define NET_PROFILE_MEMP_DESC_BYTES   24
#define NET_PROFILE_MEMP_COUNT_BYTES  8
#define NET_PROFILE_MEMP_PTR_BYTES    8
/* The N > 1 guard is not decoration.  At one lane lwipopts.h does not define
 * LWIP_MEMP_LANE at all, so none of the three tables is compiled and the honest
 * cost is 0 -- which is also the number that keeps a one-lane build byte-for-
 * byte where it was.  Summing the formula unconditionally would bill an
 * embedded build 224 bytes for tables that are not in its image. */
#define NET_PROFILE_MEMP_LANE_TABLE_BYTES                                    \
    ((CONFIG_NET_LANES > 1)                                                 \
         ? (((CONFIG_NET_LANES - 1) * NET_PROFILE_MEMP_POOLS *               \
             NET_PROFILE_MEMP_DESC_BYTES) +                                  \
            (CONFIG_NET_LANES * NET_PROFILE_MEMP_POOLS *                      \
             NET_PROFILE_MEMP_COUNT_BYTES) +                                 \
            (CONFIG_NET_LANES * NET_PROFILE_MEMP_POOLS *                      \
             NET_PROFILE_MEMP_PTR_BYTES))                                    \
         : 0)

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
