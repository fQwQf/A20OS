# xfsettingsd GTK Settings helper 生命周期修复

适用包：Alpine `xfce4-settings=4.20.1-r0`。上游的 `bus_acquired` 释放注册时保留的
helper 引用，后续总线断开时 `name_lost(NULL)` 又释放一次，消耗 main 仍持有的引用；
main 清理时因此访问已销毁对象。真实 A20OS 关机 core 已确认该调用链。

补丁以独立的 `GWeakRef` 作为总线回调数据。需要访问 helper 的回调临时取得强引用并
成对释放；对象结束时撤销 name owner，回调数据由 GIO 的 destroy notify 回收。
GSettings/xfconf 信号连接也绑定到 helper 生命周期。不跳过正常清理，不修改二进制
指令或预加载替代函数，X11、Wayland 和显示设置功能继续启用。

## 构建与镜像安装

```sh
python3 tools/packaging/xfsettingsd_fix.py --arch x86_64
```

构建脚本校验官方 4.20.1 源码的 SHA-512，使用固定 digest 的 Alpine 3.23 Docker
镜像和目标架构的 musl SDK 编译。需要可运行对应平台的 Docker、网络和现有资源门禁
要求的内存/CPU/磁盘余量；SDK 放在有大小限制的容器 tmpfs，不长期占用宿主磁盘。
解析得到的开发包版本、编译器、配置、输入和可执行文件哈希保存在
`build/upstream/xfsettingsd/<arch>/`。依赖包来自 Alpine 3.23 仓库，尚未做包仓库快照
锁定，因此固定源码/镜像摘要不等于所有后续构建逐字节相同。

`mkrootfs.py` 在 apk 安装完成后、最终显式 overlay 之前调用安装器。没有 xfsettingsd
的 world 不触发此构建；包版本不匹配则停止，要求复核补丁，不能静默应用。显式 overlay
仍保留原有覆盖优先级。构建产物有输入及二进制哈希校验的缓存。

安装器将修复后的 ELF 放到 `/usr/bin/xfsettingsd`，并在
`/usr/share/a20/xfsettingsd-lifetime/` 附带对应上游源码包、补丁、构建脚本、许可证、
构建清单与开发包版本。该修复是镜像组装时的源码覆盖，并未重新发布 Alpine APK；
在运行中的 rootfs 单独执行 `apk upgrade/fix xfce4-settings` 可能覆盖修复，必须复核
包版本并重新组装镜像或重新调用同一安装器。

脚本列出的其它目标仍需对应 Docker 平台/SDK 可用；当前实际完成构建和关机复验的
是 x86_64。不能据此计入其它架构、正式候选或 N18/N20 的验收。

## 回归证据

```sh
python3 -m unittest tools.tests.test_xfsettingsd_lifetime
```

需要已校验的源码缓存、C 编译器、GIO 开发头文件、`gdbus-codegen` 和 `dbus-daemon`。
测试提取生产回调、注册和析构代码，通过真实隔离 D-Bus 执行断开：原版提前销毁 helper
而失败，补丁版保留 main 引用且支持回调尚未分发时销毁对象。缺依赖的跳过不计覆盖。

有界证据位于 `build/evidence/v1-closeout-20261116/xfsettingsd-lifetime/`，包括源码
构建、真实安装器的版本拒绝/正确安装，以及一次 x86_64 KVM q35 SMP4 XFCE/OpenRC
正常关机。运行中已核对 ELF 哈希和 GTK Settings 导出；原总线断开警告仍出现，未再
出现同类 FATAL/core。正常关机 32.193 秒，监督器无强制超时，ext4/FAT 检查为 0。
原始失败 core 和失败报告继续保留，不重复完整浏览器/游戏矩阵。
