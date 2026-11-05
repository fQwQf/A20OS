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
