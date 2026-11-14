# Virtgpu 传输、scanout 与共享缓冲

native virtgpu 传输路径现在双向都发送固定的 virtio 线格式负载。`RESOURCE_ATTACH_BACKING` 提供 guest 页；传输命令不再携带另一份 entry 列表。DMA cache 维护覆盖已挂载的 VMO。普通资源使用零 stride/layer-stride，这是非 blob 协议的要求。host 侧校验与格式相关的 box 与偏移。

由 host 渲染的 framebuffer 通过 `SET_SCANOUT` 与 `RESOURCE_FLUSH` 呈现。CPU framebuffer 路径在 flush 之前恢复它自己的 scanout 资源，从而避免在渲染到 host texture 之后仍然显示未更新的 guest 后端。

`SUBMIT_3D` 请求一个 host GPU fence 并等待其响应。同步后端只在该 fence 真正完成之后导出已完成的 `sync_file`。对已完成的 fence，实现了输入 `sync_file`、poll、`FILE_INFO`（含 details）、`MERGE` 与 deadline 提示。目前还没有异步 fence 生产者。一个设备互斥量对提交、CPU 同步与 fence 导出排序；操作完成时唤醒等待者。

GEM backing 已使用原生固定 resident RAM 工厂；最初分配任务的 cgroup charge 和 owner pin 随 VMO 存活，导入/映射到另一文件或进程不转移 charge。创建失败、最后一个 backing 引用退出或失败清理确认后按原生 MM 契约交还。独立 DMA 页 pin 不保留预算 owner；需要预算存活的请求另持 VMO。

PRIME 导出持有活跃的 GEM 对象并映射同一个 VMO，取代了 memfd 快照与 64 项描述符表。`dup` 与 `SCM_RIGHTS` 保留该文件及其对象 pin。导入的缓冲在导出方 `GEM_CLOSE` 与 fd close 之后仍然存活。最后一个描述符关闭之后映射仍保留 VMO。导出标志使用 Linux 的 `DRM_CLOEXEC=O_CLOEXEC` 与 `DRM_RDWR=O_RDWR`。

`DMA_BUF_IOCTL_SYNC` 在 START READ 时下载 host 数据，在 END WRITE 时上传 CPU 数据。该同步后端支持 sync-file 的导出/导入与 poll。CPU 同步目前覆盖线性的 level-zero 资源；压缩格式、mip 链与数组布局需要进一步的布局跟踪。GEM 的 ID 在每 publication 内分配，handle owner 属于每个 open file；通过 PRIME/GEM_OPEN 才能建立另一文件的引用，同一文件重复导入幂等。正常最后 close 回收本文件 GEM/FB，保留其他文件、dma-buf 与 VMA 的独立存储引用。完整 master/auth、只读共享权限与命令流隔离仍未实现。不导入任意外部 DMA-BUF exporter。

设备 mmap 分派使用 `vfile_ops_t.mmap`，由 DRM 与 DMA-BUF 文件共用；syscall 层不再识别 DRM 文件。当前模块 ABI 为 12，旧模块必须重建；本轮私有 handle/FB owner 不改变公开布局。

## 覆盖范围

四 CPU 抢占式 x86_64 与 riscv64 guest 配合 host virgl 覆盖：

- native 资源分配与 96 个并发对象；
- 非均匀上传、擦除 guest 后端、下载并比对；
- 真实由 host 渲染的红、蓝像素；
- 针对 host 资源的 DMA-BUF CPU 上传与下载；
- native fence 导出、依赖导入、poll 与 `FILE_INFO`；
- 双向共享 mmap 写入、80 个并发导出、跨 fork 的 `SCM_RIGHTS`、导入方先于导出方拆除、fd 关闭后映射仍可用；
- sync-file merge、details、deadline 提示与非法参数拒绝。

`make smoke-drm-prime` 是可复用的非 virgl 共享回归门禁；同时要求 `DRM_FILE` 标记，覆盖两个独立 open 的未导入拒绝、PRIME 导入、直接 close 自动移除 framebuffer/handle 与 VMA 保留，不把既有 host virgl 历史记录当成本轮硬件渲染证据。ARM32 与 AArch64 内核构建以及七项架构边界断言也在覆盖范围内。这些是接口测试；Minecraft 的实际可玩性另行验证。

大请求、后端挂载与 fenced 提交现在使用与短命令相同的「中断优先」等待策略：订阅之后重新检查 used index，为启动阶段或 IRQ 注册失败保留有界的 poll/yield。这消除了运行时任务进入睡眠之前固定的 65,536 次 cache 同步轮询。
