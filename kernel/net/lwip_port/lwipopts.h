#ifndef A20_LWIPOPTS_H
#define A20_LWIPOPTS_H

#include "net/net_profile.h"

#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            1
#define LWIP_TIMERS                     1
#define LWIP_TIMERS_CUSTOM              0

#define LWIP_IPV4                       1
#define LWIP_IPV6                       1
#define LWIP_ICMP                       1
#define LWIP_ICMP6                      1
#define LWIP_RAW                        1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_TCP_KEEPALIVE              1
#define LWIP_DNS                        1
#define LWIP_DHCP                       1
#define LWIP_AUTOIP                     1
#define LWIP_IGMP                       1
#define LWIP_IPV6_MLD                   1
#define LWIP_IPV6_DHCP6                 1
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_MULTICAST_TX_OPTIONS       1

#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
#define LWIP_NETIF_API                  0
#define LWIP_TCPIP_CORE_LOCKING         0

/*
 * SO_REUSEADDR support.  Left at opt.h's default of 0 this port had no way to
 * honour setsockopt(SO_REUSEADDR): the socket layer stores the flag in
 * net_socket_t::reuseaddr and net_bind_reuse_allowed() consults it, but lwIP
 * never learns of it, so tcp_bind() keeps scanning the TIME-WAIT list and a
 * listener restarted on the same port gets ERR_USE (EADDRINUSE) for as long as
 * the previous connection's TIME-WAIT pcb lives -- 2 * TCP_MSL, i.e. two
 * minutes here.  Turning the switch on is only half the fix; the other half is
 * net_inet_tcp_apply_options() copying reuseaddr onto the pcb's SOF_REUSEADDR,
 * because lwIP keys every SO_REUSE decision off the pcb, not off the caller.
 * Together they make rebinding work the way Linux does: TIME-WAIT is skipped
 * for a REUSEADDR bind, and listen()/connect() re-check the 5-tuple and the
 * local address/port so a REUSEADDR bind cannot silently alias two live
 * listeners.
 */
#define SO_REUSE                        1

#define LWIP_HAVE_LOOPIF                1
#define LWIP_NETIF_LOOPBACK             1
#define LWIP_LOOPBACK_MAX_PBUFS         16
#define LWIP_NETIF_LOOPBACK_MULTITHREADING 0
#define LWIP_SINGLE_NETIF               0
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_HOSTNAME             1

#define LWIP_STATS                      1
#define LWIP_STATS_DISPLAY              0
#define LWIP_DEBUG                      0

/* Pinned on, not left to opt.h's derivation (MEMP_STATS == (MEMP_MEM_MALLOC ==
 * 0), which is 1 today).  That derivation is an accident: flipping
 * MEMP_MEM_MALLOC silently turns the counters off, and since
 * a20_lwip_format_memp() reads desc->stats unguarded, that becomes a compile
 * error rather than an empty /proc/a20/netmem.  Cost, already paid by this
 * config and unchanged by pinning: 10 bytes of .bss per declared pool plus a
 * used++/max and a used-- on every memp_malloc/memp_free, on the pbuf hot
 * path.  Accepted deliberately -- pool exhaustion is otherwise unobservable,
 * and smoke-lwip-memp asserts the resulting `err` counter stays 0. */
#define MEMP_STATS                      1

#define MEM_ALIGNMENT                   8
#define MEM_SIZE                        NET_PROFILE_MEM_SIZE
/*
 * MEMP_MEM_MALLOC selects where memp's pools live.  Left undefined it is
 * derived as 0, which builds every pool as a static array in .bss and makes
 * MEM_SIZE dead -- that is how a profile that declares a 16 KiB heap ended up
 * also carrying several hundred KiB of statically reserved pools.  Defining it
 * explicitly costs a small allocator indirection per pool operation and makes
 * the profile's MEM_SIZE the real bound.
 */
#define MEMP_MEM_MALLOC                  NET_PROFILE_MEMP_MEM_MALLOC
/* MEMP_OVERFLOW_CHECK makes memp_malloc/memp_free assert when a pool element is
 * handed out twice or freed while still referenced.  It costs a comparison per
 * pool operation, so it stays off by default and is enabled with
 * CONFIG_LWIP_MEMP_OVERFLOW_CHECK when a pbuf-accounting bug is being hunted.
 * Without it the pbuf pool has no canary at all, and memp's free list is a
 * plain pointer chain threaded through the freed blocks -- so a single double
 * free silently hands the same block to two live pbufs and every later refcount
 * reading is meaningless.  That is why an ownership bug here can stay invisible
 * for a whole session and then surface as an unrelated-looking assert. */
#ifdef CONFIG_LWIP_MEMP_OVERFLOW_CHECK
#define MEMP_OVERFLOW_CHECK            1
#else
#define MEMP_OVERFLOW_CHECK            0
#endif
/*
 * PCB and timer ceilings come from the selected profile.  The sys_timeout pool
 * must cover every PCB that can hold a slow timer, which is every established
 * PCB whenever KEEPALIVE, KEEPIDLE or KEEPINTVL is compiled in -- all three are
 * enabled below.  tcp_pcb_alloc() returns NULL once the sys_timeout pool is
 * exhausted, so an undersized pool shows up as accept() failing rather than as
 * an allocation failure, which is why the ordering is asserted instead of left
 * to be discovered under load.
 */
#define MEMP_NUM_PBUF                   (NET_PROFILE_PBUF_POOL_SIZE / 2)
#define MEMP_NUM_RAW_PCB                NET_PROFILE_RAW_PCB
#define MEMP_NUM_UDP_PCB                NET_PROFILE_UDP_PCB
#define MEMP_NUM_TCP_PCB                NET_PROFILE_TCP_PCB
#define MEMP_NUM_TCP_PCB_LISTEN         NET_PROFILE_TCP_PCB_LISTEN
#define MEMP_NUM_TCP_SEG                (NET_PROFILE_TCP_SEG_MULT * NET_PROFILE_TCP_WND_MULT)
#define MEMP_NUM_REASSDATA              16
#define MEMP_NUM_FRAG_PBUF              32
#define MEMP_NUM_ARP_QUEUE              NET_PROFILE_ARP_QUEUE
#define MEMP_NUM_IGMP_GROUP             16
#define MEMP_NUM_SYS_TIMEOUT            NET_PROFILE_SYS_TIMEOUT

_Static_assert(MEMP_NUM_SYS_TIMEOUT >= MEMP_NUM_TCP_PCB,
               "every TCP PCB can hold one sys_timeo; a smaller pool makes "
               "tcp_pcb_alloc() fail once the pool is drained");

#define PBUF_POOL_SIZE                  NET_PROFILE_PBUF_POOL_SIZE
#define PBUF_POOL_BUFSIZE               NET_PROFILE_PBUF_BUFSIZE
#define TCP_MSS                         NET_PROFILE_TCP_MSS

/*
 * lwIP leaves LWIP_WND_SCALE at 0, so the advertised window is a raw 16-bit
 * field and both directions stall at 65535 B however large TCP_WND is.  The
 * wire value is TCP_WND >> TCP_RCV_SCALE, hence TCP_WND <= 0xFFFF << the shift.
 * TCP_WND is capped by receive buffering, not by the protocol: segments park in
 * the pbuf pool, so a multiplier that keeps well under the pool's element count
 * leaves headroom rather than risking a mid-connection pool exhaustion and the
 * drops it causes.
 */
#define LWIP_WND_SCALE                  1
#define TCP_RCV_SCALE                   3
#define TCP_WND                         (NET_PROFILE_TCP_WND_MULT * TCP_MSS)
#define TCP_SND_BUF                     (NET_PROFILE_TCP_WND_MULT * TCP_MSS)
#define TCP_SND_QUEUELEN                128
#define TCP_QUEUE_OOSEQ                 1
#define TCP_LISTEN_BACKLOG              1
#define TCP_DEFAULT_LISTEN_BACKLOG      16

/* lwIP truncates the advertised window to 16 bits after shifting; a larger
 * TCP_WND would silently wrap rather than negotiate a wider window. */
_Static_assert(TCP_WND <= (0xFFFF << TCP_RCV_SCALE),
               "TCP_WND must fit the 16-bit window field after TCP_RCV_SCALE");

/* A full-size segment plus its Ethernet, IP and TCP headers must fit one
 * PBUF_POOL element; lwIP carves the headers out of the head pbuf's payload. */
_Static_assert(TCP_MSS + 54 <= PBUF_POOL_BUFSIZE,
               "PBUF_POOL_BUFSIZE must leave room for headers above TCP_MSS");

/* The receive window is only reachable if the pool can hold that much payload
 * queued at once, otherwise the window advertises capacity that cannot be
 * stored and the shortfall shows up as mid-connection drops. */
_Static_assert(NET_PROFILE_TCP_WND_MULT <= NET_PROFILE_PBUF_POOL_SIZE,
               "TCP_WND exceeds what the pbuf pool can hold queued");

/*
 * The pbuf pool must be allocatable out of the heap the profile declared.
 * MEMP_MEM_MALLOC=1 makes memp_malloc() call mem_malloc() per element instead of
 * reserving a static array, so PBUF_POOL_SIZE bounds occupancy rather than
 * reserving it: every element in flight is a claim on MEM_SIZE.  When the ceiling
 * exceeds the heap the pool cannot reach its declared size, MEMP_STATS reports
 * err > 0, and the shortfall surfaces as unexplained receive drops.
 *
 * MEM_ALIGNMENT stands in for the element header, because memp.c builds a
 * PBUF_POOL element as LWIP_MEM_ALIGN_SIZE(sizeof(struct pbuf)) +
 * LWIP_MEM_ALIGN_SIZE(PBUF_POOL_BUFSIZE) and struct pbuf does not exist yet at
 * this point in the include chain.  So this term is deliberately optimistic --
 * 520 B against a measured 536 B on the riscv64 tier 1 build -- because
 * understating the element is the wrong direction to fail in.  Do not "fix" it to
 * 24 without re-measuring; the ceiling then stops holding.  Holds on all three
 * profiles: 12480/16384, 395264/524288, 6324224/16777216.
 *
 * Pbuf pool only.  The other thirteen pools this config compiles in also draw on
 * MEM_SIZE and their element sizes are lwIP struct layouts not visible yet; on
 * riscv64 tier 1 they add 12640 B at full occupancy against 3904 B left, so
 * tier 1's full set of pool ceilings is not simultaneously satisfiable within
 * tier 1's own MEM_SIZE (25696 B of claims against 16384 B).  That needs the
 * ceilings lowered, not an assertion added.
 */
_Static_assert(PBUF_POOL_SIZE * (PBUF_POOL_BUFSIZE + MEM_ALIGNMENT) <= MEM_SIZE,
               "MEM_SIZE cannot back the pbuf pool at its declared "
               "PBUF_POOL_SIZE; with MEMP_MEM_MALLOC=1 the ceiling is a claim "
               "on the heap, so the pool would cap below the configured size");

#define IP_REASSEMBLY                   1
#define IP_FRAG                         1
#define IP_REASS_MAX_PBUFS              32
#define LWIP_IPV6_REASS                 1
#define LWIP_IPV6_FRAG                  1
/* 64-bit targets cannot fit lwIP's IPv6 reassembly helper into IP6_FRAG_HLEN. */
#define IPV6_FRAG_COPYHEADER            1
#define LWIP_IPV6_AUTOCONFIG            1
#define LWIP_IPV6_SEND_ROUTER_SOLICIT   1

#define DHCP_DOES_ARP_CHECK             1
#define LWIP_DHCP_DOES_ACD_CHECK        1
#define DNS_MAX_SERVERS                 2
#define DNS_TABLE_SIZE                  8
#define DNS_MAX_NAME_LENGTH             256

#define PPP_SUPPORT                     0
#define PPPOE_SUPPORT                   0
#define PPPOS_SUPPORT                   0
#define PPPOL2TP_SUPPORT                0

/* Compile in the PCB list sanity checker.  tcp_priv.h stubs tcp_pcbs_sane() to a
   constant 1 unless one of the TCP_DEBUG* switches is set, and TCP_DEBUG_PCB_LISTS
   is what selects the checking form of TCP_REG/TCP_RMV, so define it here rather
   than enabling the noisy debug switches. */
#if CONFIG_NET_PCB_SANE
#define TCP_DEBUG_PCB_LISTS 1
#endif

/*
 * Turn the documented core-lock discipline into something the CPU enforces.
 *
 * lwIP ships LWIP_ASSERT_CORE_LOCKED() as an empty macro
 * (src/include/lwip/opt.h:227) unless the port defines it, which is why the
 * lock contract used to be prose only: the ~50 sites in tcp.c/tcp_in.c/
 * raw.c/udp.c all expanded to nothing, so a path that touched PCB lists
 * without g_lwip_lock corrupted them silently.  docs/net/net-lanes.md
 * ("为什么没有任何断言拦住它") records the concrete damage that let through.
 *
 * The owner is recorded as a CPU id rather than a boolean because a boolean
 * answers "is this flag set", which is true on every CPU while *another* CPU
 * holds the lock -- exactly the case the assertion exists to catch.  The
 * __builtin is evaluated at the macro's expansion point, so `site` is the
 * return address inside the lwIP function that ran unlocked.
 */
#if CONFIG_NET_LOCK_ASSERT
void a20_lwip_assert_core_locked(void *site);
#define LWIP_ASSERT_CORE_LOCKED() \
    do { a20_lwip_assert_core_locked(__builtin_return_address(0)); } while (0)
#else
#define LWIP_ASSERT_CORE_LOCKED()
#endif

#define LWIP_RAND()                     ((u32_t)random_u64())

#endif /* A20_LWIPOPTS_H */
