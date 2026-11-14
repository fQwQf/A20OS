# DRM 发现与挂载感知的符号链接

stock libdrm 会解析 `/sys/dev/char/226:{0,128}`、枚举物理 GPU 的 `drm` 目录并读取其总线身份。此前的目录与伪造的 PCI 身份使 Mesa 无法发现 DRM 设备。

DRM class 条目、字符设备条目与总线设备条目现在链接到同一棵物理树。PCI 名字与配置字节来自所选 GPU 经 `pci_device_location` 枚举到的 function；MMIO GPU 使用 platform bus 与其注册的设备名。物理树同时暴露 card0 与 renderD128。生成的 sysfs 子节点保留其挂载点，因此属性读取走 sysfs 操作，而不是缓存一个长度为零的合成文件。这是针对所选显示 GPU 的发现支持，不是完整 PCI 枚举或热插拔支持。

GPU 名字、PCI 配置/uevent/modalias 和 framebuffer 属性的内容生成使用一次带引用的 DISPLAY class access；身份与 ops 来自同一绑定，promotion 不会在同一次生成中混用设备。生成完成即交还 active call/class 引用，文件只保存复制的内容；不同属性的独立 open 不保证观察同一 GPU。离线读取不能通过旧 publication 跳到新绑定。它不提供完整热拔插语义，RAM framebuffer 持久 VMA 已独立保留 VMO；DRM 文件已在 open 时固定 publication，兼容 GEM/virgl/CRTC/EDID 状态按 publication 分区，旧文件不随默认选择或重绑切换；发现节点仍为默认聚合入口，尚非多 GPU 枚举。旧 get_fb 物理视图与厂商 BO/GPUVM 仍需进一步协议，见 [GPU publication 契约](../drivers/guide/native-runtime.md#gpu-publication-与作用域调用)。

通用 VFS 现在对普通 lookup 与 openat2 使用同一个挂载感知的符号链接遍历器：绝对链接从进程根重新开始并保留剩余分量；`RESOLVE_IN_ROOT` 锚定在传入目录上；`RESOLVE_BENEATH` 拒绝绝对链接。显式传入所选挂载点还能保持 bind mount 身份。

覆盖范围（四 CPU、启用抢占）：

- x86_64：`symlink_mount_test`、`drm_sysfs_test`、`cpuinfo_test 4`；实际 GPU 路径为 `/sys/devices/pci0000:00/0000:00:02.0`。
- riscv64：上述用例加 `resource_ns_test`；实际 MMIO GPU 路径为 `/sys/devices/platform/virtio-gpu1`。
- x86_64 `vfs_stress`：无缺失或被跳过的覆盖项。

可复用门禁：`make smoke-symlink-mount`、`make smoke-drm-sysfs`；`make smoke-gpu-access-x86_64` 另覆盖双 CPU generic 2D GPU 的 sysfs 发现及 MMU framebuffer mmap/ioctl/刷新。
