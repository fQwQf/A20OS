# External Dependency Integration Facts

This document records current integration contracts for imported or externally
built components. It describes present behavior, not future plans.

## lwIP

- `EXTERNAL_LWIP_SOURCE_MANIFEST`: kernel lwIP sources live in `kernel/external/lwip/sources.mk`; update that single manifest when importing a new lwIP tree.
- `EXTERNAL_LWIP_CONFIG_CONTRACT`: A20OS uses lwIP in `NO_SYS=1` mode through `kernel/net/lwip_stack.c`, not lwIP's socket API.
- The kernel socket layer owns socket files and translates to lwIP TCP/UDP/RAW primitives behind `g_lwip_lock`.
- Progress is polling based: `sys_check_timeouts()`, virtio-net TX completion, RX delivery, and `netif_poll()` are driven through `a20_lwip_poll()` and `kernel_progress_poll()`.

## QEMU Networking Defaults

- `EXTERNAL_QEMU_NET_DEFAULTS`: **Resolved.** `10.0.2.15`, `10.0.2.2`, and `10.0.2.3` are no longer compiled into the kernel or user commands.
- Network configuration is command-line / runtime config only. The kernel command line accepts:
  - `a20.ip=<IPv4>`
  - `a20.netmask=<IPv4>`
  - `a20.gateway=<IPv4>`
  - `a20.dns=<IPv4>` (repeatable)
  - `a20.dhcp=1` (overrides static IP/gateway/netmask)
  - `a20.hostname=<name>`
- If no keys are present and DHCP is not enabled, the interface is brought up without an IPv4 address; socket calls requiring a route return `-ENETUNREACH`.
- The effective configuration is exposed read-only via `/proc/net/config` as `ip=`, `netmask=`, `gateway=`, `dns0=`, `dhcp=`, and `hostname=` lines.
- QEMU boot scripts preserve existing behavior by appending `a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os`.

## Userland Imports

- `EXTERNAL_USERLAND_UPGRADE_CHECKLIST`: after changing musl, sbase, or mksh sources/build rules, run Linux syscall smoke, shell smoke, and coreutils smoke before accepting the upgrade.
- `EXTERNAL_STATIC_LINK_REBUILD_CONTRACT`: musl, ABI wrapper, startup, or syscall layout changes require rebuilding all statically linked user programs; stale binaries are detected by `user/build/.build-id` and Makefile source timestamp checks.

## TLSe and wget

- `EXTERNAL_TLSE_WGET_LIMITS`: TLSe/wget are compatibility tools, not a complete modern HTTPS stack.
- Current integration does not claim TLS 1.3, a modern cipher policy, or full certificate ecosystem behavior; network documentation and tests must not treat wget success as proof of complete TLS support.
