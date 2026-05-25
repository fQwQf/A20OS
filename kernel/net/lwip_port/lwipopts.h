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
#define MEM_SIZE                        (128 * 1024)
#define MEMP_NUM_PBUF                   64
#define MEMP_NUM_RAW_PCB                16
#define MEMP_NUM_UDP_PCB                32
#define MEMP_NUM_TCP_PCB                32
#define MEMP_NUM_TCP_PCB_LISTEN         16
#define MEMP_NUM_TCP_SEG                96
#define MEMP_NUM_REASSDATA              16
#define MEMP_NUM_FRAG_PBUF              32
#define MEMP_NUM_ARP_QUEUE              32
#define MEMP_NUM_IGMP_GROUP             16
#define MEMP_NUM_SYS_TIMEOUT            32

#define PBUF_POOL_SIZE                  64
#define PBUF_POOL_BUFSIZE               1536
#define TCP_MSS                         1460
#define TCP_WND                         (8 * TCP_MSS)
#define TCP_SND_BUF                     (8 * TCP_MSS)
#define TCP_SND_QUEUELEN                32
#define TCP_QUEUE_OOSEQ                 1
#define TCP_LISTEN_BACKLOG              1
#define TCP_DEFAULT_LISTEN_BACKLOG      16

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
