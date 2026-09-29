# 宿主侧工具：ABI 门禁、virglrenderer 构建、启动前资源检查

三个脚本都跑在宿主上，不需要 QEMU 里的 guest 配合。

| 工具 | 作用 | 需要 root | 跑在 |
|---|---|---|---|
| [`tools/check-drm-abi.sh`](../../tools/check-drm-abi.sh) | DRM ioctl 号 + 结构体布局对 Linux UAPI 的门禁 | 否 | 宿主 |
| [`tools/build-virglrenderer.sh`](../../tools/build-virglrenderer.sh) | 构建宿主 virglrenderer ≥ 0.11 | 是 | 宿主 |
| [`tools/a20_preflight.py`](../../tools/a20_preflight.py) | `tools/a20 run/debug/test` 启动前的宿主资源门禁 | 否 | 宿主 |

---

## 1. `tools/check-drm-abi.sh` — DRM UAPI 门禁

### 为什么需要它

`kernel/include/drivers/gpu/drm.h` 把 ioctl 号写成字面十六进制（它们落在
switch 里，不走宏）。于是一个写错的常量不会报错：驱动的 switch 匹配不上，
ioctl 落到 `default` 分支，用户态看到 `EINVAL`/`ENOTTY`。症状指向 Mesa，
病灶是一个头文件里的一个字符。

这不是假想。写这个门禁的原因就是：这些号被人眼逐条核对过，而人眼核对一次
都没有发现它们漂移。已查出的缺陷与症状分析见
[3d-graphics.md §8.1](3d-graphics.md#81-本轮查出的-uapi-abi-缺陷错的-ioctl-常量是隐形的)。

### 它检查两件事

1. ioctl 号：把 `drm.h` 里的 `DRM_IOCTL_*` 名字抓出来，针对本机安装的
   `/usr/src/linux-headers-*/include/uapi/drm` 编译一个探针，打印上游的值
   再逐条比对。
2. 结构体布局：从 `kernel/drivers/gpu/drm.c` 里把结构体定义 lift 出来
   编译，量 `sizeof` 与关键字段的 `offsetof`。正确的 ioctl 号配错误的布局
   照样静默破坏数据：libdrm 按 UAPI 说的偏移写字段，驱动按自己那份结构体
   读，两边都"成功"。

第 2 步不可省。已知实例：`struct drm_gem_open` 的 handle 被放在 offset 8，
而 Linux 在 offset 4 返回它，于是每次 `GEM_OPEN` 都从错误字段读 handle。

### 用法

```bash
tools/check-drm-abi.sh          # 退出码即结论
```

- exit 0：全部匹配（若有几个号无上游对应，会在摘要里说明）。
- exit 1：有漂移，逐条打印 `ours=` 与 `linux=`。
- skip / exit 0：宿主没装 Linux UAPI 头，或没有 C 编译器。此时它
  什么都没检查，不要把 skip 当作通过。

### 当前状态

实测 48 个号与 Linux UAPI 一致，1 个无上游对应（`MODE_DPMS`，上游同样
没有定义）。**双向可证伪已验证**：把 `MODE_RMFB` 扰动一下、或从
`drm_mode_fb_cmd` 删掉一个字段，都会被抓出来。

### 它不能证明什么

它不启动 guest，不验证任何运行时行为。它只保证线格式（号 + 布局）与 Linux
UAPI 一致。渲染语义、呈现、宿主 renderer 都不在它的范围内。

---

## 2. `tools/build-virglrenderer.sh` — 宿主 renderer

### 为什么需要它

virglrenderer 跑在宿主上：QEMU `dlopen()` 它，把 guest 的 GL 命令流喂进去。
所以 3D 可用性由宿主的 virglrenderer 决定，不是 guest 内核。

一个旧到建不出 `virgl_renderer_get_capset()` 所需的离屏 desktop GL context
的 virglrenderer，会让 Mesa 的 `virtio_gpu_dri.so` attach 失败并返回
`ERR_INVALID_PARAMETER`。改 A20OS 解决不了这件事。

发行版仍只发 1.1.0（2020）的不少；Debian trixie 没有更新候选。所以只能自己编。
这个脚本需要 root，因此由操作者主动运行，不是构建流程偷偷做的事。

### 子命令

```bash
tools/build-virglrenderer.sh check    # 报告宿主状态与缺失依赖（只读，不需要 root）
tools/build-virglrenderer.sh build    # 取源码、构建、安装到 tools/virgl/install   [需要 root]
tools/build-virglrenderer.sh env      # 打印要用的 LD_LIBRARY_PATH
```

`check` 报告四段：宿主 QEMU 版本、两版 renderer（本地 prefix 与发行版包）、
`/dev/dri/renderD128` 是否真的可访问、以及构建依赖清单。

> 一个存在但不可读的 render 节点会产生看起来像驱动 bug 的 QEMU 失败。
> 所以先跑 `check`。判断权限用 `test -r` / `test -w`，不要对字符设备做
> `head -c1`（会阻塞）。

### 当前状态：**已运行；renderer 已升级，但 Mesa 仍未 attach**

已实测（`fca72f5f`）：

- 依赖 `libgbm-dev`、`libdrm-dev`、`libudev-dev`、`python3-mako`、
  `python3-yaml`（1.3.0 的 gallium 会 import，漏掉它 `check` 会过、构建两分钟后才炸）
  已装齐；
- 脚本从源码构建出 virglrenderer 1.3.0（发行版那份是 2020 年的 1.1.0-2），
  装在 `tools/virgl/install`；
- `LD_DEBUG=libs` 确认 **QEMU 加载的确实是这份新库**。

但 **stock Mesa 仍未能 attach**。QEMU 依旧不向 guest 提供
`VIRTIO_GPU_F_VIRGL`：NVIDIA EGL 下它静默降级为 2D，强制 Mesa EGL 则显式报
`eglInitialize failed: EGL_NOT_INITIALIZED` / `render node init failed`。
强制 Mesa 时 `eglinfo -p surfaceless` 是成功的（radeonsi / OpenGL 4.6），
而 `eglinfo -p gbm` 失败。卡点落在 GBM/设备平台这条路上。

所以可以说宿主 renderer 已升级，但**不可以说 stock Mesa 已能 attach**。
完整判据表见 [gpu-3d-roadmap.md §5.1](gpu-3d-roadmap.md)。

### 装了之后怎么生效

Makefile 在 `tools/virgl/install/lib/libvirglrenderer.so.1` 存在时自动通过
`LD_LIBRARY_PATH` 拾取该前缀（`QEMU_LIBPATH`），所以既有 make 目标不变：

```bash
make ARCH=riscv64 GPU_3D=1 QEMU_MEMORY=2G run-world-gui
```

装到别处就显式覆盖：`VIRGL_PREFIX=/some/where`。

### 唯一可证伪的判据：guest 日志里的 capset 体积

```
[GPU] virtio-gpu 3D (virgl): capset[0] id=1 ver=1 size=NNN
```

- NNN 是数 KB → 生效了，现代协议在用。
- NNN = 308 → 还在加载旧库，构建没起作用。

308 字节是决定性线索：现代 capset 要描述 shader 能力、格式表、参数上限，
只能是 KB 量级。

> 按 QEMU 自己的文档，OpenGL passthrough 需要 virglrenderer 0.8.2+，
> Venus（virtio-vulkan）需要 1.0.0+。本脚本默认构建 `VIRGL_VERSION=0.11.2`。

---

## 3. `tools/a20_preflight.py` — 启动前的宿主资源门禁

### 为什么需要它

`tools/a20 run|debug|test` 跑 GUI 或 4G 实例时会把宿主榨干：QEMU 要几 GB 的
guest RAM 和若干 vCPU，构建还要写 4 GiB 镜像。宿主本来就忙的时候，结果不是
一次干净的失败：宿主被 OOM kill、构建被换页，或者一个时序敏感的门禁因为与
被测代码无关的原因失败。

> 忙碌宿主上的每一次"测试 flaky"报告，都应当**先怀疑宿主，再怀疑测试**。

所以启动路径先采样宿主；实例放不下时**等待**资源回来，而不是直接失败。默认
等待，因为最常见的原因是另一个 agent 或同租户任务跑完了，它会自己解决；立刻
放弃只是把重试推给人。

### 环境变量

| 变量 | 作用 |
|---|---|
| `A20_PREFLIGHT=0` | 跳过整个门禁 |
| `A20_PREFLIGHT_TIMEOUT=<秒>` | 给等待设上界（`0` = 不等，只报告） |
| `A20_PREFLIGHT_VERBOSE=1` | 通过时也打印采样结果 |

### 覆盖范围

只作用于 `run` / `debug` / `test`，即会启动 guest 的路径。
`build` / `package` / `flash` 不启动 guest，因此不施加门禁。

### 与 Makefile 的关系

门禁镜像三个 Makefile 默认值（内存、GUI 内存、vCPU），以便实例没显式设置时
算出来的需求与启动器一致。它**故意不在运行时解析 Makefile**；
`tools/a20 check` 会重新读取 Makefile 并在副本漂移时报错。
默认等待 15 分钟（`DEFAULT_WAIT_SECONDS`），轮询间隔 15 秒。

阈值：内存按声明值的 1.5 倍再留 1 GiB 余量；负载每核不超过 1.0（TCG guest
被过度时间分片会触发看起来像内核挂死的 watchdog 超时）；磁盘至少留 8 GiB。

---

## 4. 三者的共同点

- 都在宿主上跑，不需要 guest 配合，也不需要 root（除 `build`）。
- 每一个都能失败，并且可证伪方向都实测过。这就是本仓库的门禁标准：
  不能失败的检查不构成验证。
- skip 与 pass 必须区分：三个脚本在没有依赖时都会明确说 skip，不要把 skip
  读成通过。
