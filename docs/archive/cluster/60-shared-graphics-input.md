# 共享内核中的真实图形与输入

在系统自带 x86_64 QEMU/KVM 上，两个独立进程各提供一个 vCPU，共享同一 A20OS 内核与 RAM。只有主实例连接 GPU、键盘、鼠标和 payload 磁盘；CPU 1 上的普通 Linux 程序通过已有 DRM、DMA-BUF、fence、evdev 和文件系统接口使用这些设备。没有增加应用专用集群接口，也没有编译或修改 QEMU。

## 已完成的验证

`cluster_threads --console --gpu-input` 将测试子进程绑定 CPU 1，从主实例磁盘挂载并 exec 现有 `gpu3d_test`。测试实际提交两次 virgl 清屏命令，分别回读 4096 个红色和蓝色像素，并验证 DMA-BUF CPU 上传/下载、原生 fence 导出/导入/poll，以及 96 个并存对象。退出码 77（跳过）不能作为通过。

随后 CPU 1 打开 `/dev/input/event*` 并 poll/read。宿主经主实例 QMP 注入键盘 A 按下/释放、鼠标左键按下/释放及相对位移 (17, -9)，guest 必须收到对应真实设备事件并保持 CPU 1 亲和性。此路径经过 virtio 输入设备及既有输入驱动，没有直接向 guest evdev 队列写入测试结果。

原有 pthread 迁移、TLS/栈保持、128 次实际权限异常恢复、1024 项动态领取计算、条件变量超时和唯一串口收发同时通过。普通双核单实例使用相同应用作对照。这次改动只扩展测试工具和测试程序，没有修改默认内核路径。

## 复现与证据

需要已安装且支持 virgl 的 QEMU/KVM、可用的 `/dev/dri/renderD128`，以及项目现有构建依赖。当前脚本的图形显示参数使用此 render node。

```sh
python3 tools/check-multi-qemu-kernel.py --workload gpu-input \
  --out .kernel-build/cluster/multi-qemu/gpu-input-new --timeout 180 --jobs 4
python3 tools/check-multi-qemu-kernel.py --mode smp --workload gpu-input \
  --out .kernel-build/cluster/multi-qemu/gpu-input-smp-new --timeout 180 --jobs 4
```

仅测试 GPU 时使用 `--workload gpu`。图形用例启用现有 embedded 驱动配置，由主 CPU 在启动阶段初始化设备。独立证据目录保留冻结 ELF、ROM、源码差异、哈希、QMP CPU/内存信息、共享 RAM 映射、只读 payload 和串口日志。临时 RAM 在本次 QEMU 子进程全部停止后删除。

证据位于 `.kernel-build/cluster/multi-qemu/`：

| 运行目录 | 模式 | 结果 |
| --- | --- | --- |
| owner-gpu-01 | 双实例共享内核 | CPU 1 图形回读及全部 GPU 检查通过 |
| owner-gpu-baseline-01 | 普通 SMP | 相同 GPU 检查通过 |
| owner-gpu-input-01 | 双实例共享内核 | GPU 与真实键盘鼠标事件通过 |
| owner-gpu-input-baseline-01 | 普通 SMP | GPU 与真实键盘鼠标事件通过 |

`graphics-input-owner-evidence.json` 汇总并独立核对上述四次运行的冻结内核、 GPU 程序、payload 和日志哈希。共享输入运行的 report 尚未单列 input_events；其冻结 source.diff 中保存了实际注入参数，普通对照 report 另记录事件数组。

## 架构范围和下一步

**Minecraft 已知可运行环境是 x86_64。** [52](52-main-minecraft-integration.md)和 [53](53-linux-application-execution.md) 的客户端菜单证据均属于 x86_64 单实例；[59](59-shared-jvm-execution.md) 的单 JVM 跨实例证据和本页图形输入证据同样仅覆盖 x86_64。本页阶段尚未验收客户端；后续已在 x86_64 完成跨两个实例的菜单、输入和正常退出，见 [61](61-shared-minecraft-client.md)。

AArch64、RISC-V、STM32 等平台继续共用内核集群接口与设备抽象，但公共机制通过不代表 Minecraft 可运行。其 JVM、LWJGL、本地动态库、图形栈及平台资源可能需要额外移植，必须各自验证，不能继承 x86_64 的客户端支持结论。

后续客户端验证见 [61](61-shared-minecraft-client.md)。当前 embedded 初始化结果没有覆盖任意 CPU 上的动态 PCI 探测： PCI 配置空间等直接访问仍需 owner 路由完善。物理多 MCU、混合指针 ABI 的共享堆、设备 owner 故障恢复和普通模式性能基准也仍未验收。
