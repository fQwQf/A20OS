# Network Configuration Design

This document defines the runtime network configuration model for A20OS. It
replaces the hard-coded QEMU user-network defaults in
`kernel/net/lwip_stack.c` and the hard-coded DNS addresses in user commands.

## Design Decision

Network configuration is **command-line / runtime config only**. There are no
compile-time board defaults for IP address, netmask, gateway, DNS, DHCP, or
hostname. Every board, emulator, or deployment must supply these values at
boot through the kernel command line, or configure them at runtime through the
interface described below.

The reasons for this decision are:

- QEMU user-network defaults such as `10.0.2.15`, `10.0.2.2`, and `10.0.2.3`
  are development conveniences, not architecture constants.
- Real boards and non-QEMU backends need different addresses.
- Runtime configuration lets the same kernel image boot on multiple networks
  without rebuilding.
- It keeps the kernel free from per-board `#ifdef` address tables.

## Kernel Command-Line Keys

The kernel recognizes the following command-line keys. All keys are optional.

| Key | Value | Example |
|-----|-------|---------|
| `a20.ip` | IPv4 address in dotted decimal | `a20.ip=10.0.2.15` |
| `a20.netmask` | IPv4 netmask in dotted decimal | `a20.netmask=255.255.255.0` |
| `a20.gateway` | IPv4 default gateway | `a20.gateway=10.0.2.2` |
| `a20.dns` | IPv4 DNS server, may be given multiple times | `a20.dns=10.0.2.3` |
| `a20.dhcp` | `1` to enable DHCP, `0` to disable | `a20.dhcp=1` |
| `a20.hostname` | Hostname string, max 63 bytes | `a20.hostname=a20os` |

### Key rules

- `a20.dhcp=1` takes precedence over static `a20.ip`, `a20.netmask`, and
  `a20.gateway`. The kernel runs DHCP and uses the lease to populate the
  runtime config.
- If `a20.dhcp=0` or `a20.dhcp` is absent, the kernel uses the static values.
- `a20.dns` may appear multiple times. The first occurrence populates DNS
  server slot 0, the second slot 1, and so on, up to the lwIP DNS server limit.
- `a20.hostname` is copied into the loopback netif and any Ethernet netif
  created after the key is parsed.
- All values are parsed once during early boot and stored in a runtime
  `a20_net_config` structure.

## Parsing Location

The kernel parses the network keys in the same early command-line pass that
handles other boot arguments. The parsed values are stored in a single
structure:

```c
typedef struct a20_net_config {
    ip4_addr_t ip;
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    ip4_addr_t dns[LWIP_DNS_SERVER_LIST_SIZE];
    int        dns_count;
    int        dhcp_enable;
    char       hostname[64];
} a20_net_config_t;
```

The storage location is `kernel/net/net_config.c` (new file). The structure is
initialized to zeros and then populated by the command-line parser. The
existing `a20_lwip_register_virtio_netifs()` in
`kernel/net/lwip_stack.c` reads from this structure instead of using the
hard-coded QEMU addresses.

## Fallback Behavior

The fallback behavior is explicit and safe:

- If no network keys are present and `a20.dhcp` is absent, the kernel treats
  the interface as unconfigured. The netif is still registered and brought up
  at the link level, but it has no IPv4 address, no gateway, and no DNS
  servers.
- Network system calls that require a route return `-ENETUNREACH` until a
  valid configuration is supplied.
- If `a20.dhcp=1` is present but the DHCP exchange does not complete before
  userland starts, the interface remains unconfigured until the exchange
  finishes. Userland must tolerate transient `-ENETUNREACH` errors or wait
  for a config-ready signal.
- If `a20.dhcp=0` and only some static keys are present, the missing values
  remain zero. For example, `a20.ip` without `a20.netmask` leaves the netmask
  at `0.0.0.0`.

There is no hidden fallback to QEMU addresses. The previous hard-coded
`10.0.2.15`/`10.0.2.2`/`10.0.2.3` defaults are removed.

## Runtime Configuration Interface

In addition to the command line, the kernel exposes the runtime configuration
through `/proc/net/config`:

```text
ip=10.0.2.15
netmask=255.255.255.0
gateway=10.0.2.2
dns0=10.0.2.3
dhcp=1
hostname=a20os
```

The file is read-only and reflects the current effective configuration. If
DHCP is enabled, the values update when the lease changes.

A future syscall pair, `sys_net_get_config` and `sys_net_set_config`, may
replace the proc file for atomic updates. The initial implementation uses the
proc file to avoid adding new syscalls before the lock contract is stable.

## Consumption in User Commands

The following user commands consume the runtime configuration.

### `wget`

- Removes the hard-coded `DNS_SERVER_IP` constant.
- Reads the first DNS server from `/proc/net/config`.
- If no DNS server is configured, `wget` prints an error and exits before
  sending any query.
- All other behavior, including URL parsing, TCP connection, TLS handshake,
  and HTTP fetch, remains unchanged.

### `ping`

- Removes the hard-coded `DNS_SERVER_IP` constant.
- Reads the first DNS server from `/proc/net/config` when a hostname is given.
- If the argument is already an IPv4 literal, no DNS lookup is performed.
- If no DNS server is configured and the argument is a hostname, `ping` prints
  an error and exits.

### `udpsend`

- `udpsend` takes the destination IPv4 address and port as command-line
  arguments, so it does not need a DNS lookup.
- It still depends on the interface being configured. If the interface has no
  address or route, the kernel `sendto()` path returns `-ENETUNREACH`.
- `udpsend` may add an optional `-i` flag in the future to print the current
  network config from `/proc/net/config`, but the default behavior stays the
  same.

## DHCP Interaction with Locking

DHCP runs as part of lwIP timeout processing. It holds `g_lwip_lock` while
updating netif addresses and DNS server state. To avoid surprising userland,
address changes made by DHCP are committed directly to the lwIP netif and to
the runtime `a20_net_config` structure while `g_lwip_lock` is held.

Userland reads `/proc/net/config` without holding any kernel spinlock. The
proc read path copies the config values into a temporary buffer under a short
critical section. A reader may see a slightly stale value if DHCP updates the
config concurrently, which is acceptable for command-line tools.

## Migration Checklist

When implementing this design, verify each item:

- [ ] `kernel/net/lwip_stack.c` no longer contains hard-coded QEMU addresses.
- [ ] `a20_lwip_register_virtio_netifs()` reads from `a20_net_config`.
- [ ] Command-line parser recognizes all six `a20.*` keys.
- [ ] `a20.dhcp=1` triggers DHCP and overrides static values.
- [ ] `/proc/net/config` exposes the effective configuration.
- [ ] `user/cmds/wget.c` reads DNS from `/proc/net/config`.
- [ ] `user/cmds/ping.c` reads DNS from `/proc/net/config`.
- [ ] `user/cmds/udpsend.c` tolerates `-ENETUNREACH` when unconfigured.
- [ ] QEMU boot scripts pass `a20.ip=10.0.2.15 a20.netmask=255.255.255.0
      a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os` to preserve
      existing smoke behavior.
- [ ] Network smoke tests cover both configured and unconfigured boot paths.
