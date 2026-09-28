#ifndef A20_LWIPOPTS_H
#define A20_LWIPOPTS_H

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

#define MEM_ALIGNMENT                   8
#define MEM_SIZE                        (512 * 1024)
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
#define MEMP_NUM_PBUF                   256
#define MEMP_NUM_RAW_PCB                16
#define MEMP_NUM_UDP_PCB                32
#define MEMP_NUM_TCP_PCB                64
#define MEMP_NUM_TCP_PCB_LISTEN         16
#define MEMP_NUM_TCP_SEG                384
#define MEMP_NUM_REASSDATA              16
#define MEMP_NUM_FRAG_PBUF              32
#define MEMP_NUM_ARP_QUEUE              32
#define MEMP_NUM_IGMP_GROUP             16
#define MEMP_NUM_SYS_TIMEOUT            32

#define PBUF_POOL_SIZE                  256
#define PBUF_POOL_BUFSIZE               1536
#define TCP_MSS                         1460

/*
 * lwIP leaves LWIP_WND_SCALE at 0, so the advertised window is a raw 16-bit
 * field and both directions stall at 65535 B however large TCP_WND is.  The
 * wire value is TCP_WND >> TCP_RCV_SCALE, hence TCP_WND <= 0xFFFF << the shift.
 * TCP_WND is capped by receive buffering, not by the protocol: segments park in
 * the pbuf pool, so 64 * MSS (~91 KiB, ~64 of 256 bufs) leaves 4x headroom
 * rather than risking a mid-connection pool exhaustion and the drops it causes.
 */
#define LWIP_WND_SCALE                  1
#define TCP_RCV_SCALE                   3
#define TCP_WND                         (64 * TCP_MSS)
#define TCP_SND_BUF                     (64 * TCP_MSS)
#define TCP_SND_QUEUELEN                128
#define TCP_QUEUE_OOSEQ                 1
#define TCP_LISTEN_BACKLOG              1
#define TCP_DEFAULT_LISTEN_BACKLOG      16

/* lwIP truncates the advertised window to 16 bits after shifting; a larger
 * TCP_WND would silently wrap rather than negotiate a wider window. */
_Static_assert(TCP_WND <= (0xFFFF << TCP_RCV_SCALE),
               "TCP_WND must fit the 16-bit window field after TCP_RCV_SCALE");

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
