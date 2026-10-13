# Resource namespaces

`/proc/<pid>/ns/{uts,ipc,net,cgroup}` now identifies real, referenced namespace
objects. `clone`/`clone3` create memberships, ordinary fork inherits them,
`unshare` creates a new domain, and `setns` joins the object pinned by an open
namespace descriptor. Namespace owners are fixed user namespace references;
opening a descriptor from a more privileged task does not change ownership.
Combined NEWUSER requests create that domain before the resource namespaces.
Allocation failures roll back namespace memberships and credentials.

| Domain | Isolated resources |
| --- | --- |
| UTS | Hostname/domainname through uname, setters and procfs |
| IPC | SysV message queues, semaphore sets, shared-memory IDs/keys, POSIX MQ names and counts |
| Network | Socket port/listener lookup, abstract Unix addresses, loopback flags/MTU, route/link and socket diagnostic dumps, procfs network views, newly mounted sysfs network views |
| Cgroup | Namespace root, relative procfs cgroup paths and the root of newly mounted cgroupfs views |

Existing SHM mappings and MQ descriptors remain usable after changing IPC
namespace. The last namespace reference removes its SysV resources and unlinks
its MQ names; mappings and open MQ files retain their backing storage. Sockets
keep their creation namespace across setns, fork and accept. A newly created
network namespace contains only a down loopback device; bringing it up enables
private IPv4/IPv6 TCP and UDP. It has no physical NIC, external route or DNS
configuration. Physical network events cannot leak into it. Existing mounts keep
their mount-time network view; a new sysfs mount captures the new namespace.

These changes do not provide veth devices or physical NIC migration. IPC and
socket storage still use the kernel's bounded shared allocation pools. Namespace
procfs descriptors retain the repository's readable `type:[inode]` format;
nsfs magic-link and namespace-owner ioctl compatibility are separate work.
The interfaces are retained and backed by isolation; unsupported operations
return errors rather than succeeding without isolation.

Run `make -j8 smoke-resource-ns-x86_64` and `make -j8 smoke-resource-ns`.
Both execute the same resource namespace regression plus existing mount, PID,
user namespace and procfs tests. Coverage includes cross-domain denial, repeated
last-reference IPC teardown, namespace descriptor lifetime, old SHM/MQ/socket
handles, IPv4/IPv6 loopback, inherited membership, mount roots and capability
ownership. These gates have passed on x86_64 (4 CPUs, preemption) and RISC-V.
`make -j8 smoke-vm-fault-x86_64` also covers real hardware GS loads/stores,
fork/exec/COW, sparse unmapping and no_new_privs/seccomp ABI behavior.
