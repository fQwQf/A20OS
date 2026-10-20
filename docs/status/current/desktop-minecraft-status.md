# 桌面与 Minecraft 当前边界

> 仍未完成的事项见 [1.0 缺口台账](../gaps/README.md)；本页只描述当前状态。

来源：原 `docs/distro/minecraft.md` 的「当前边界」一节，以及同一轮实现记录的结论。迁移时删除了
日期、镜像哈希与逐次运行日志，只保留仍然成立的边界。

- 854×480、4 区块视距实测可以移动和操作背包；观察到约 8–18 FPS，菜单约
  20 FPS。这是功能验证中的现场读数，仍需性能优化，未达到稳定 30/60 FPS。
- OpenAL 使用 musl 系统库，sound engine 能启动；当前配置的设备是 No Output，
  尚未验收有声播放。原始 glibc C++ OpenAL 的异常路径不再进入 JVM。
- LWJGL C native 仍经 gcompat；启动器选择系统 allocator。没有宣称完整 glibc
  ABI 兼容，也没有为 jemalloc 注入补丁。
- Wayland 不支持绝对设置鼠标位置的 GLFW 提示仍可见；键盘移动与背包已经实测，
  鼠标转向和跳跃也已实测；全屏及真实硬件输入尚未系统验收。
- 默认 token 为 0，服务端会返回真实的 401；不把它误报为 DNS/TLS 故障。
  当前没有集成 Microsoft 账号登录，不宣称 Realms 或多人服务器可用。
- GEM 句柄命名空间仍是设备级，尚未实现 per-open 隔离；fence 当前同步完成，
  不是异步 GPU scheduler；DMA-BUF CPU 同步目前覆盖线性 level-0 纹理。

[组件状态](../../distro/minecraft-components.md) 记录 JVM、native 和音频的具体范围。

## 实现范围

内核侧为 Minecraft 提供的修复都经通用接口提供，没有按进程名或 ISA 名特判：

- CPU 信息报告实际在线 CPU 数，架构身份由 `kernel/arch` hook 提供。
- 普通路径与 openat2 共用挂载感知的符号链接遍历；GPU sysfs 暴露真实 PCI/MMIO 身份，
  动态属性走文件 read，Mesa 可正确发现 GPU。
- `VIRTGPU_RESOURCE_CREATE` 分配独立 GEM 对象；PRIME/DMA-BUF 导出保留真实 VMO，mmap 与
  `SCM_RIGHTS` 共享同一对象。
- 3D 上传/下载使用正确 wire 布局，scanout 使用宿主 3D 资源，提交等待真实 host GPU fence。
- 大命令与 backing attachment 使用完成中断唤醒，保留启动阶段的有限轮询。
- `SIOCGIFCONF` 的 NULL 尺寸查询与输出缓冲区语义已修正，Java 能正常枚举网卡。

详见 [GPU 共享与同步](../../graphics/virtgpu-sharing-and-sync.md)、
[GPU 发现](../../graphics/drm-sysfs-discovery.md) 与
[网卡枚举](../../kernel/linux-abi/interface-enumeration.md)。
