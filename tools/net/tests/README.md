# KVM Multi-Guest Network Integration Tests (`tools/net/tests/`)

This directory contains automated end-to-end network topology and stack integration test harnesses executing under KVM.

## Files

| File | Subsystem | Description |
|------|-----------|-------------|
| `test-bsd-bootstrap-kvm.py` | BSD Stack | Boots real A20OS with BSD loopback diagnostic under KVM |
| `test-bsd-desktop-kvm.py` | BSD Stack | Boots full XFCE desktop image with BSD network backend and verifies curl/socket traffic |
| `test-bsd-internet-kvm.py` | BSD Stack | Boots BSD stack with virtio/SLIRP and verifies internet socket connectivity |
| `test-net-control-kvm.py` | Netlink | Exercises production netlink route/link/address control plane using stock Alpine iproute2 |
| `test-net-router-kvm.py` | NAT Router | Boots 3-node topology (Linux LAN clients → A20OS router → Linux WAN) over socket interconnects |
| `test-bsd-router-soak.py` | F-Stack Router | Runs concurrent TCP, UDP, and ICMP from two Linux LAN guests through a DHCP-configured WAN |

## F-Stack router soak

Use the Linux kernel and Kea peer initramfs from the [IPv6 PD test](../../../docs/net/router.md#自动发布委派前缀).
The soak starts the Linux guests in shell mode and adds a static echo benchmark
and a BusyBox DHCP server configuration to their initramfs.

```sh
python3 tools/net/tests/test-bsd-router-soak.py \
  --kernel <A20OS-router-kernel.bin> --linux <Linux-kernel> \
  --peer-initrd <peer-initramfs.cpio.gz> --busybox <static-BusyBox> \
  --out <new-evidence-directory>
```

The default run uses 30 minutes of concurrent traffic on VirtIO-net.
Use `--nic-model e1000e` for emulated Intel adapters. The TCP and UDP results
record one direction of verified payload throughput and echo round-trip latency.
The UDP probe records replies that miss its 100 ms window as losses and keeps
sending. Late echoes do not count as replies to later probes. The test records
ICMP loss independently and samples guest memory, F-Stack backing allocations,
NIC counters, and filter and conntrack counters. It then drops WAN carrier
while the two clients send probes, checks DHCP and fresh TCP/UDP traffic after
link recovery, and checks idle conntrack reclamation. The manifest records
samples as the run progresses. The QEMU links use loopback UDP and require no
host network configuration.

To exercise the physical PC image under OVMF, first package it with
`tools/a20 package router-x86_64-pc -- RAMFS_DISTRO=1`. Replace
`--kernel` in the command with `--router-image <a20os-router-uefi.img>`
and add `--ovmf-code <OVMF_CODE.fd> --ovmf-vars <OVMF_VARS.fd>`.
This mode verifies the production router tools and starts WAN configuration
from the serial console.
