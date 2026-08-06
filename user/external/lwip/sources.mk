# lwIP source manifest. Update this single list when importing a new lwIP tree.
LWIPDIR := user/external/lwip/src
LWIP_SRC = \
    $(LWIPDIR)/core/init.c \
    $(LWIPDIR)/core/def.c \
    $(LWIPDIR)/core/dns.c \
    $(LWIPDIR)/core/inet_chksum.c \
    $(LWIPDIR)/core/ip.c \
    $(LWIPDIR)/core/mem.c \
    $(LWIPDIR)/core/memp.c \
    $(LWIPDIR)/core/netif.c \
    $(LWIPDIR)/core/pbuf.c \
    $(LWIPDIR)/core/raw.c \
    $(LWIPDIR)/core/stats.c \
    $(LWIPDIR)/core/sys.c \
    $(LWIPDIR)/core/altcp.c \
    $(LWIPDIR)/core/altcp_alloc.c \
    $(LWIPDIR)/core/altcp_tcp.c \
    $(LWIPDIR)/core/tcp.c \
    $(LWIPDIR)/core/tcp_in.c \
    $(LWIPDIR)/core/tcp_out.c \
    $(LWIPDIR)/core/timeouts.c \
    $(LWIPDIR)/core/udp.c \
    $(LWIPDIR)/core/ipv4/acd.c \
    $(LWIPDIR)/core/ipv4/autoip.c \
    $(LWIPDIR)/core/ipv4/dhcp.c \
    $(LWIPDIR)/core/ipv4/etharp.c \
    $(LWIPDIR)/core/ipv4/icmp.c \
    $(LWIPDIR)/core/ipv4/igmp.c \
    $(LWIPDIR)/core/ipv4/ip4.c \
    $(LWIPDIR)/core/ipv4/ip4_addr.c \
    $(LWIPDIR)/core/ipv4/ip4_frag.c \
    $(LWIPDIR)/core/ipv6/dhcp6.c \
    $(LWIPDIR)/core/ipv6/ethip6.c \
    $(LWIPDIR)/core/ipv6/icmp6.c \
    $(LWIPDIR)/core/ipv6/inet6.c \
    $(LWIPDIR)/core/ipv6/ip6.c \
    $(LWIPDIR)/core/ipv6/ip6_addr.c \
    $(LWIPDIR)/core/ipv6/ip6_frag.c \
    $(LWIPDIR)/core/ipv6/mld6.c \
    $(LWIPDIR)/core/ipv6/nd6.c \
    $(LWIPDIR)/netif/ethernet.c
