# NET 缺口

对应 V1-NET。现状见 [current/net-status.md](../current/net-status.md)、[current/server-readiness.md](../current/server-readiness.md) §八。

当前没有未完成的 1.0 NET 缺口。

有意不做的设计边界写在参考文档里，不在此处：零拷贝收包（设备 recv op 只提供拷贝，见 [net/network-config-design.md](../../net/network-config-design.md)）、ICMP 差错报文跟踪与 PMTU、分片 NAT、ND6/SLAAC 地址通知、IPv6 NAT、e1000/rtl8139 的多 lane 中断入口（驱动 owned，见 [net/network-lock-contract.md](../../net/network-lock-contract.md)）。
