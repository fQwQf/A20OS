# SIOCGIFCONF output semantics

Minecraft's OpenJDK 21 reported `SocketException: Invalid argument
(ioctl(SIOCGIFCONF) failed)` while Netty enumerated network interfaces. This was
an ioctl compatibility defect, independent of DNS and TCP connectivity.

`ifc_buf == NULL` asks for the required byte count. Its incoming `ifc_len` is
irrelevant; OpenJDK sets the pointer without initializing that length. With a
non-NULL buffer, rows are output only. Their previous `sa_family` contents are
not a family filter. A negative or sub-row capacity returns zero complete rows
without writing the buffer. The initial and isolated network namespaces follow
the same capacity semantics.

The implementation now follows Linux's
[dev_ifconf](https://github.com/torvalds/linux/blob/master/net/core/dev_ioctl.c)
and [inet_gifconf](https://github.com/torvalds/linux/blob/master/net/ipv4/devinet.c).
The triggering caller is OpenJDK's
[enumIPv4Interfaces](https://github.com/openjdk/jdk21u/blob/master/src/java.base/unix/native/libnet/NetworkInterface.c).
No Java- or Minecraft-specific kernel branch is used.

`net_iface_test` tests a NULL query with negative incoming length, a dirty row
buffer, truncation, negative capacity with an untouched sentinel, and discovered
interface getter round trips. The same 24 checks pass on host Linux and on
4-CPU preemptible x86_64/KVM and RISC-V guests. The old x86_64 kernel fails six
checks with the new executable. Evidence is under `build/browser-followup/`:
`net-iface-host.log`, `net-ifconf-{x86,riscv64}-guest.log` and
`net-ifconf-x86-negative-guest.log`.

The durable runtime gate is `make smoke-net-iface`; the executable can also run
from either architecture's dev image as `net_iface_test`. This change does not
add IPv6 address enumeration or alter which interfaces each namespace owns.
