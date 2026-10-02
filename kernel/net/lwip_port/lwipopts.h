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

#define LWIP_RAND()                     ((u32_t)random_u64())

#endif /* A20_LWIPOPTS_H */
