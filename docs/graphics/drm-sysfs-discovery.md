# DRM 发现与挂载感知的符号链接

stock libdrm 会解析 `/sys/dev/char/226:{0,128}`、枚举物理 GPU 的 `drm` 目录并读取其总线身份。此前的目录与伪造的 PCI 身份使 Mesa 无法发现 DRM 设备。

DRM class 条目、字符设备条目与总线设备条目现在链接到同一棵物理树。PCI 名字与配置字节来自所选 GPU 经 `pci_device_location` 枚举到的 function；MMIO GPU 使用 platform bus 与其注册的设备名。物理树同时暴露 card0 与 renderD128。生成的 sysfs 子节点保留其挂载点，因此属性读取走 sysfs 操作，而不是缓存一个长度为零的合成文件。这是针对所选显示 GPU 的发现支持，不是完整 PCI 枚举或热插拔支持。

通用 VFS 现在对普通 lookup 与 openat2 使用同一个挂载感知的符号链接遍历器：绝对链接从进程根重新开始并保留剩余分量；`RESOLVE_IN_ROOT` 锚定在传入目录上；`RESOLVE_BENEATH` 拒绝绝对链接。显式传入所选挂载点还能保持 bind mount 身份。

覆盖范围（四 CPU、启用抢占）：

- x86_64：`symlink_mount_test`、`drm_sysfs_test`、`cpuinfo_test 4`；实际 GPU 路径为 `/sys/devices/pci0000:00/0000:00:02.0`。
- riscv64：上述用例加 `resource_ns_test`；实际 MMIO GPU 路径为 `/sys/devices/platform/virtio-gpu1`。
- x86_64 `vfs_stress`：无缺失或被跳过的覆盖项。

可复用门禁：`make smoke-symlink-mount` 与 `make smoke-drm-sysfs`。
