# guest 写盘后 ext4 位图校验和不一致

> 来源：原 `docs/status/current/known-issues.md` 的对应小节；该文件已拆分删除。
> 关联台账：[FS-09](../fs.md)。

`image-world` 产物 `e2fsck -fn` 干净；但 guest 启动一次后，从宿主机看（QEMU `-snapshot` 的 qcow2
overlay，或 kill 后的镜像）会出现 `Block bitmap checksum does not match bitmap`，`debugfs` 直接打不开。

这解释了「测试环境注意事项」里那条「损坏来源未定位」：**损坏不是构建期引入的，是 guest 写入时产生的**。
损坏的 rootfs 会伪造内核 bug（udev worker 超时、应用起不来），排查前务必先 `e2fsck -fn`。

怀疑 A20OS 的 ext4 写入路径没有同步 `metadata_csum` 的位图校验和（`^has_journal` 下没有日志回放来兜底）。
需要在内核 ext4 写路径上核对 group descriptor/bitmap 的 checksum 更新。
