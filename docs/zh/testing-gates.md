# 测试入口

本页列出仓库中保留的测试与构建检查。源码文本断言、文档行号漂移检查和生成物新鲜度检查已移除；这里只保留构建、行为测试和真实契约检查。

## 快速测试（宿主侧，无需 QEMU）

```bash
make check-manifests      # instances/、components/、Makefile 配置一致
make check-a20-tests      # a20 工具链与内核辅助逻辑的单元测试
make check-smoke-cases    # smoke 用例表与 make 目标一致
make host-tests           # 宿主 gcc 编译并运行内核格式/辅助逻辑单测
make check-cluster-unit   # 集群协议、MCU 运行时与有界作业测试
make check-cluster-fuzz   # 集群编解码与运行时 ASan/UBSan
make check-cluster-wakeup # 集群收包唤醒与 park 竞态
make check-slab-cpu-window
make check-virtio-net-completion
make check-abi-boundary   # Linux syscall 覆盖表与登记表一致
make check-drm-abi        # DRM ioctl 编号与线布局对照 Linux UAPI
```

`make check` 依次运行上面这组快速测试。

## 构建检查

```bash
make check-kernel-build        # 当前 ARCH/BOARD 的 bring-up 构建
make check-user-build          # 当前 ARCH 的用户态构建
make check-build-matrix        # 多架构内核 + 用户态构建
make check-<arch>-bringup      # 单架构内核 bring-up
make check-<arch>-user         # 单架构用户态
make check-stm32f103           # STM32F103 固件构建
make check-mcu-program-rtl     # ARMv7-M 程序与 RTL 联合构建
```

## QEMU 行为测试

`smoke-*` 和 `check-cluster-*` 在 QEMU 中运行真实 guest。常用入口：

```bash
make smoke-riscv64        # BRINGUP 启动阶段与自动关机
make smoke-abi-linux      # Linux ABI syscall 冒烟
make smoke-mm-stress      # MM/COW 压力
make smoke-vfs-stress     # VFS 并发与一致性
make check-cluster-send   # 单向 SEND 与路由
```

完整目标列表可用 `make -qp | grep '::'` 查询。

## 1.0 验收

功能与性能验收方法见 [releases/v1.0-validation.md](releases/v1.0-validation.md)。当前未完成的交付项见 [status/gaps/](status/gaps/README.md)。
