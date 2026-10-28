#ifndef _NET_TUN_H
#define _NET_TUN_H

/*
 * Kernel side of the Linux TUN ABI (docs/drivers/classes/tun.md).
 *
 * TUN is a kernel service, not a DriverStore module: there is no hardware to
 * probe, instances are created by open("/dev/net/tun") + TUNSETIFF, and the
 * data path is lwIP <-> a character-device queue.  The netif slots live in
 * lwip_stack.c so their .bss is covered by the per-profile netif budget; the
 * instance state and the ABI live in drivers/net/tun.c.
 */
struct netif;
struct vfile;

/* Register/unregister the lwIP netif backing one instance.  NULL means the
 * profile's TUN slot count is exhausted. */
struct netif *a20_lwip_tun_add(void);
void a20_lwip_tun_remove(struct netif *n);

/* 1 when `n` is one of the TUN netifs; the netlink snapshot uses this for the
 * interface name and to keep IFF_LOOPBACK off. */
int a20_lwip_netif_is_tun(struct netif *n);

/* ABI entry points used by devfs and the netlink snapshot. */
int tun_file_open(struct vfile *vf);
int a20_tun_netif_name(struct netif *n, char *out, unsigned cap);
/* Resolve a user-chosen TUN name ("tailscale0") to its netif; NULL when
 * no live instance carries it. */
struct netif *a20_tun_netif_by_name(const char *name);

#endif
