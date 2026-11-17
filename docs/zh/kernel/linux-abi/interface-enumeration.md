# `SIOCGIFCONF` 的输出语义

Minecraft 的 OpenJDK 21 在 Netty 枚举网络接口时曾报 `SocketException: Invalid argument (ioctl(SIOCGIFCONF) failed)`。这是 ioctl 兼容性缺陷，与 DNS、TCP 连通性无关。

`ifc_buf == NULL` 表示询问所需字节数，此时传入的 `ifc_len` 无关紧要——OpenJDK 只设置指针，不初始化这个长度。缓冲区非 NULL 时该调用只做输出：行内原有的 `sa_family` 内容不构成 family 过滤条件。容量为负或不足一行时返回零条完整记录，且不写缓冲区。初始网络命名空间与隔离出来的网络命名空间遵循同一套容量语义。

实现按 Linux 的 [`dev_ifconf`](https://github.com/torvalds/linux/blob/master/net/core/dev_ioctl.c) 与 [`inet_gifconf`](https://github.com/torvalds/linux/blob/master/net/ipv4/devinet.c) 处理；触发方是 OpenJDK 的 [`enumIPv4Interfaces`](https://github.com/openjdk/jdk21u/blob/master/src/java.base/unix/native/libnet/NetworkInterface.c)。内核里没有 Java 或 Minecraft 专属分支。

`net_iface_test` 覆盖：NULL 查询且传入长度为负、脏的行缓冲区、截断、容量为负且哨兵值不被写入，以及已发现接口 getter 的往返。持久门禁是 `make smoke-net-iface`，该可执行文件也可在两套架构的 dev 镜像里直接运行（`net_iface_test`）。本改动不增加 IPv6 地址枚举，也不改变各命名空间拥有哪些接口。
