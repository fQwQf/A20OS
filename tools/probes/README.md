# Hardware, Hypervisor & Network Probes

This directory contains standalone host probes, socket loopback validators, ABI conformance probes, and hypervisor communication drivers.

## Contents

### 1. Network Interface & Controller Probes
- `e1000_host_probe.py`: Hostfwd loopback probe asserting traffic round-trip through guest Intel e1000/e1000e NICs.
- `rtl8139_host_probe.py`: Realtek RTL8139 host-side packet round-trip verifier.
- `lwip_lane_host_probe.py`: Parallel hostfwd multi-lane TCP traffic generator.
- `net_udp_host_probe.py`: Host-side UDP echo and packet integrity probe.

### 2. NAT & Conntrack Verifiers
- `netnat_host_probe.py`: Hostfwd DNAT port-forwarding verification probe.
- `netnat_snat_probe.py`: Host loopback responder for outbound guest SNAT verification.
- `netnat_result.py`: Strict result parser for network smoke logs and comparison markers.

### 3. ABI & Standalone C Probes
- `net-connection-probe.c`: Host/guest scalable connection tracking probe.
- `net-icmp-router-probe.c`: Minimal ICMP router reachability probe.
- `net-socket-abi-probe.c`: Linux/A20OS socket ABI conformance probe.
- `net-tun-abi-probe.c`: TUN character device FIFO, MTU, and connection probe.

### 4. Hypervisor & Porting Probes
- `hyp_pair_probe.py`: Two-guest live isolation, console, and shutdown lifecycle probe for the Type-1 hypervisor.
- `probe-bsd-core.py`: Out-of-tree BSD object compiler and partial-linker probe.
- `vport_host_probe.py`: Host chardev socket probe for VirtIO serial/console validation.
