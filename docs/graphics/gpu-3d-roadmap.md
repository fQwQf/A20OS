# A20OS GPU 3D 路线图（virtio-gpu / Mesa 挂载）

本文是 GPU 3D 能力的**实现路线图与事实基线**，配套阅读
[3d-graphics.md](3d-graphics.md)（原理与内核接口细节）和
[drivers/classes/display.md](../drivers/classes/display.md)（显示设备模型）。

> 状态结论以 `kernel/drivers/gpu/` 的 HEAD 源码与
> `kernel/include/drivers/gpu/` 的 UAPI 头为准。本文不重复"已实现"清单，
> 那在 [3d-graphics.md §0](3d-graphics.md)。

---

## 1. 一个必须先纠正的前提：目标 UAPI 是 VIRTGPU，不是 VIRGL

社区里讨论 virtio-gpu 3D 时容易说"实现 `DRM_IOCTL_VIRGL_*`"。**对现代 Mesa
这是错的**，会直接把工作引向一条死路。

Mesa 的 `virtio_gpu_dri.so`（Alpine `mesa-dri-gallium` 里的
`/usr/lib/dri/virtio_gpu_dri.so`）走的是 **`virtgpu_drm.h`** 里的
`DRM_IOCTL_VIRTGPU_*` 家族，命令号落在 `DRM_COMMAND_BASE`（0x40）之后的
**0x41–0x4b**，类型字母仍是 `'d'`：

```
DRM_VIRTGPU_MAP                  = 0x40 + 0x01
DRM_VIRTGPU_EXECBUFFER           = 0x40 + 0x02
DRM_VIRTGPU_GETPARAM             = 0x40 + 0x03
DRM_VIRTGPU_RESOURCE_CREATE      = 0x40 + 0x04
DRM_VIRTGPU_RESOURCE_INFO        = 0x40 + 0x05
DRM_VIRTGPU_TRANSFER_FROM_HOST   = 0x40 + 0x06
DRM_VIRTGPU_TRANSFER_TO_HOST     = 0x40 + 0x07
DRM_VIRTGPU_WAIT                 = 0x40 + 0x08
DRM_VIRTGPU_GET_CAPS             = 0x40 + 0x09
DRM_VIRTGPU_RESOURCE_CREATE_BLOB = 0x40 + 0x0a
DRM_VIRTGPU_CONTEXT_INIT         = 0x40 + 0x0b
```

`DRM_IOCTL_VIRGL_*`（类型字母 `'A'` = 0x41）是更早的 virgl UAPI，现代 Mesa
的 virtio-gpu 驱动**不再使用**。A20OS 目前两者都没有实现。

权威来源：内核头
`/usr/src/linux-headers-*/include/uapi/drm/virtgpu_drm.h`（本机已核对）。

展开后的 ioctl 号（`_IOWR('d', nr, size)`，即
`(3<<30)|(size<<16)|(0x64<<8)|nr`），可直接照抄：

| ioctl | nr | size | 用途 |
|---|---|---|---|
| `0xc0106441` | 0x41 | 16 | `MAP`（mmap 偏移） |
| `0xc0406442` | 0x42 | 64 | `EXECBUFFER`（提交命令流） |
| `0xc0106443` | 0x43 | 16 | `GETPARAM`（能力探测） |
| `0xc0386444` | 0x44 | 56 | `RESOURCE_CREATE` |
| `0xc0106445` | 0x45 | 16 | `RESOURCE_INFO` |
| `0xc02c6446` | 0x46 | 44 | `TRANSFER_FROM_HOST` |
| `0xc02c6447` | 0x47 | 44 | `TRANSFER_TO_HOST` |
| `0xc0086448` | 0x48 | 8 | `WAIT` |
| `0xc0186449` | 0x49 | 24 | `GET_CAPS`（capset 转交） |
| `0xc030644a` | 0x4a | 48 | `RESOURCE_CREATE_BLOB` |
| `0xc010644b` | 0x4b | 16 | `CONTEXT_INIT` |

注意这些号与 A20 已有的 `DRM_IOCTL_*`（nr ≤ 0xbc，类型 `'d'`）不冲突。

### 1.1 A20OS 内核是"运输层"，不解析命令流

这一点决定了工作量的边界。分工是：

| 环节 | 由谁完成 |
|---|---|
| GLSL → SPIR-V | **Mesa**（guest 用户态） |
| SPIR-V → virgl 命令流 | **Mesa** |
| 命令流字节 → host virglrenderer | **A20OS 内核**（原样透传） |
| virglrenderer → host GPU 执行 | **QEMU + virglrenderer**（宿主机） |

所以 A20OS **不需要**实现 GLSL 编译器、SPIR-V 工具链、光栅化器，也**不需要**
知道 virgl 命令流的字节格式——那些字节由 Mesa 生成，内核只搬运。内核需要做
对的只有三件事：

1. 暴露 `DRM_IOCTL_VIRTGPU_*`，让 Mesa 能探测能力、建 context、建资源、提交；
2. 3D 资源要有 **backing**（`RESOURCE_ATTACH_BACKING`），否则 host 侧资源
   无内存可读写，任何渲染都是空转；
3. GEM 对象模型正确（**本轮已完成**），这是 GBM 与 VIRTGPU 两条路径的共同地基。

"开发着色器"在 A20OS 的正确含义是**建运输管道**，不是建编译器。

---

## 2. QEMU 侧：能力早已具备，此前一个都没用

宿主机（本机实测）：

- `/dev/kvm` 存在，x86_64 走 KVM
- QEMU 10.0.13 同时提供 `virtio-gpu-gl-pci` 与 `virtio-gpu-gl-device`
- `libvirglrenderer.so.1` 已安装
- 宿主 GPU：Radeon 780M，radeonsi，GL 4.6 可用

**但改之前**，所有 GUI 实例用的都是 2D-only 设备（`virtio-gpu-device` /
`virtio-gpu-pci`），host 从不提供 `VIRTIO_GPU_F_VIRGL`，于是
`A20_GPU_IOCTL_VIRGL_CHECK` 必然 `-ENXIO`。全树 `virgl=on` / `virtio-gpu-gl`
零命中。

**现状（本轮已落地）**：`GPU_3D=1` 选择 `-gl-` 变体，2D 仍是默认值，因此没有
任何既有调用发生变化。

```bash
make ARCH=riscv64 GPU_3D=1 QEMU_MEMORY=2G run-world-gui
make ARCH=x86_64  GPU_3D=1 QEMU_MEMORY=4G run-world-gui
```

注意 virglrenderer 跑在**宿主机**，所以 TCG guest 也能拿到宿主 GPU 渲染——
这是 riscv64 / aarch64 / arm32（无 KVM）唯一能验证 3D 的途径。但宿主机需要
有可用的 GL/EGL；无头环境用 `QEMU_GUI_DISPLAY=egl-headless`。

`machine.extra_qemu` 此前在 `run` 路径是**死代码**（只有 `a20_test.py` 处理
它），实例清单无法给 `tools/a20 run` 加设备；现已统一为 Makefile 的
`EXTRA_QEMU`，Makefile 仍是命令行的唯一来源。

---

## 3. 已完成：GEM 对象模型

GBM 无法创建 rendering buffer 的根因是**根本没有 GEM 对象模型**，只有
dumb buffer 这一条老路。本轮已按 Linux 的模型补齐：

- `drm_buffer_t` → `drm_gem_t`，条目 16 → 64
- 新增 `GEM_CREATE` / `GEM_MMAP` / `GEM_OPEN` / `GEM_GET_HANDLE`
- `GEM_CLOSE` 改为**真正释放**（此前是刻意的 no-op，会泄漏每个 GEM handle；
  对合成器的少数 dumb buffer 无所谓，对分配上千对象的渲染器致命）
- dumb buffer 改为复用同一分配器，不再重复"找空槽 + 发 handle"
- handle 分配改为设备全局计数（per-open 计数会与全局 store 撞号）
- 新增 `MODE_GETFB2`（libdrm 探测期会发，此前是 EINVAL 空洞）
- `GEM_MMAP` 复用 `handle << PAGE_SHIFT` 编码，故 `drm_linux_mmap()` 一条
  路径同时服务 dumb buffer 与 GEM 对象

### 3.1 一个容易踩的坑：`DRM_CAP_*` 有两套编号

`DRM_IOCTL_GET_CAP` 用的是 libdrm 的**legacy** 编号，它与内核
`include/uapi/drm/drm.h` 里的 `DRM_CAP_*` **顺序不同**：

```
legacy (GET_CAP 实际使用)          uapi (不同顺序，别混用)
DRM_CAP_DUMB_BUFFER          0x1   DRM_CAP_DUMB_PREF_SHARED  1
DRM_CAP_VBLANK_HIGH_CRTC     0x2   DRM_CAP_DUMB               2
DRM_CAP_DUMB_PREFERRED_DEPTH 0x3   DRM_CAP_VBLANK_HIGH_CRTC   3
DRM_CAP_DUMB_PREFER_SHADOW   0x4   DRM_CAP_DUMB_PREF_SHADOW   4
DRM_CAP_PRIME                0x5   DRM_CAP_PREFERRED_MAJOR    5
DRM_CAP_TIMESTAMP_MONOTONIC  0x6   DRM_CAP_USER_PREFERRED_... 6
DRM_CAP_ASYNC_PAGE_FLIP      0x7   DRM_CAP_DRAW_MESH          7
```

`drm_get_cap()` 现有的表是**正确**的（已按 libdrm 头逐条核对）。若有人"顺手"
按 uapi 顺序重排，PRIME 与 dumb buffer 发现会被静默关闭，且不会有任何报错。

---

## 4. 待完成：DRM_IOCTL_VIRTGPU_* + 3D backing

这是把"内核能透传 3D"变成"Mesa 能用 3D"的一步。按依赖顺序：

### 4.1 第一步：能力探测（让 Mesa 不再直接放弃）

Mesa 的 virtio-gpu 驱动上来就 `DRM_IOCTL_VIRTGPU_GETPARAM` 问
`VIRTGPU_PARAM_3D_FEATURES` / `VIRTGPU_PARAM_CONTEXT_INIT` /
`VIRTGPU_PARAM_CAPSET_QUERY_FIX` / `VIRTGPU_PARAM_RESOURCE_BLOB` /
`VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs`。**答错或不答，驱动直接判定本设备无 3D**，
后面全部不发生。这一步最便宜、收益最直接。

`VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs` 是位图，bit *n* 表示 capset id *n* 可用；
host 侧信息已有（`virtio_gpu_get_capset_info()`），但目前只用于开机日志。

### 4.2 第二步：capset 转交（`VIRTGPU_GET_CAPS`）

`virtio_gpu_get_capset()` 已实现（含大响应缓冲路径），但被
`__attribute__((unused))` 挂起。`GET_CAPS` 需要把它接到用户态提供的地址上。
注意：`virgl_renderer_config` 结构体布局必须与 host virglrenderer 一致，**不要
凭记忆写**，按 virglrenderer 源码或 Mesa 内 `virgl_hw.h` 的副本逐字段核对。

### 4.3 第三步：3D 资源 + backing（本轮的真正硬骨头）

`RESOURCE_CREATE_3D` 目前**不挂 backing**，host 侧资源没有任何内存。这条修好
之前，即使命令流被接受也是空转。

正确形态（与 Linux 一致）：VIRTGPU 资源由一个 **GEM handle** 承载，内核把
该 GEM 的 VMO 页转成 `virtio_gpu_mem_entry[]`（`pfn_to_phys(vmo_peek_page(...))`）
再发 `RESOURCE_ATTACH_BACKING`。基础设施已就位：

- `vmo_peek_page(vmo, index) -> pfn_t`
- `pfn_to_phys(pfn_t) -> paddr_t`（`kernel/include/mm/frame.h:117`）
- `struct virtio_gpu_resource_attach_backing` / `virtio_gpu_mem_entry`
  已在 `virtio_gpu.h`

注意驱动接口分层：`gpu_dev_ops_t.ioctl()` 走的是 `copy_from_user`，而 backing
数组是内核构造的物理地址，**不能**走这条路。建议在 `gpu_dev_ops_t` 上加一个
3D 专用 op（`resource_attach_backing`），把 virtio-gpu 细节留在
`virtio_gpu.c`，GEM 语义留在 `drm.c`。

### 4.4 第四步：提交（`VIRTGPU_EXECBUFFER`）

把 `drm_virtgpu_execbuffer.command` / `.size` 指向的用户命令流原样送
`VIRTIO_GPU_CMD_SUBMIT_3D`。现有 `virtio_gpu_submit_3d()` 已处理
DMA 可见性与三段描述符链，**但它当前由私有 `A20_GPU_IOCTL_SUBMIT_3D` 驱动，
且上限 128 KiB**——Mesa 的命令流经常更大，上限需要复核。

### 4.5 第五步：retire 私有 ABI

`A20_GPU_IOCTL_*`（`virtio_gpu.h:272-277`）是 A20 私有 fork。一旦 VIRTGPU
UAPI 可用，它就没有存在理由：留着等于维护两套 3D ABI。届时删除，并同步修正
`docs/graphics/3d-graphics.md` 的 §2 与 §4。

---

## 5. 验证门禁：必须能失败

旧 `gpu3d_test` 有两个致命问题，二者都让它**在任意配置下都是绿的**：

1. 定义了 `A20_GPU_IOCTL_SUBMIT_3D` 却**从未调用**，只建了个空 16×16 纹理；
2. 2D-only 设备上打印 `skipping 3D path` 后 `return 0`。

规则（本轮确立，替代旧做法）：**一个"已验证"结论必须指向一个可以失败的门禁；
不能失败的检查不构成验证。**

因此：

- `gpu3d_test` 的退出码要三态：`0` = 3D 已验证、`77` = SKIP（autotools 约定，
  2D-only 设备）、`1` = FAIL。SKIP 不得与 PASS 混淆。
- 新增 `instances/smoke-gpu3d-*.toml`，`expect` 断言**真实提交的**结果。
  `tools/a20 test` 这条路径已经支持（`[test].commands` + `expect`）。
- 文档里的 PASS 只在附上产生它的命令、配置与提交时有效。

---

## 6. QEMU 测试矩阵

| 目标 | 实例 | 备注 |
|---|---|---|
| 内核 3D 透传 | `GPU_3D=1` + `smoke-gpu3d-*` | 需要宿主 virgl |
| Mesa 3D 挂载 | `GPU_3D=1` + xfce world | 需完整镜像，最重 |
| 桌面回归 | `GPU_3D=0` + `xfce-*` | 每次 DRM 改动必跑 |
| 引导 | `smoke-riscv64` / `smoke-abi-linux` | 便宜，可频繁跑 |

**迭代目标选择**：x86_64 + KVM 是唯一快的环境（LWJGL 也只提供
x86_64/aarch64 native，Minecraft 本来就只能在这两个架构上跑）。riscv64 只有
TCG，适合验证 3D **通路**，不适合验证性能。

已知约束：`instances/xfce-x86_64.toml` 把 `smp` 钉在 1（注释称 smp>1 在
x86_64 挂起）。这让唯一快的环境失去多核，**建议单独立项排查**，否则后续所有
性能判断都不可信。

---

## 7. 明确的非目标（QEMU-first 范围内不做）

- **着色器编译器 / SPIR-V 工具链**：Mesa 的职责。
- **自研 DRI 驱动**：走 VIRTGPU UAPI 让 stock Mesa 挂载，而不是自己实现一份。
- **真机 GPU 驱动**（Mali / i915 / VC4 / amdgpu / nouveau）：对着 QEMU-only
  目标是多月工程。
- **Vulkan ICD**：需要 VIRTGPU 全量 + zink/lavapipe，差得更远。
- **硬件视频解码**：QEMU virtio-gpu 没有编解码引擎，VA-API 无从谈起。
  可做的只是修 `docs/distro/mpv-luajit-crash.md` 那个已确定性复现的崩溃，
  让 ffplay 软解稳定；virgl 通了之后 `vo=gpu` 能跑，但仍是软解。
- **XWayland 窗口不呈现**：与 GPU 无关的独立 bug（labwc-xwm 集成），却挡住
  所有 X11 应用含 Minecraft。已有三方对照与通过的对照组
  （[3d-graphics.md §9.22](3d-graphics.md)），建议并行修，优先级高于 GPU 工作。

---

## 8. 建议顺序

1. 修 `gpu3d_test` 的三态退出码 + 真实提交（本轮未做，成本最低、防自欺）
2. `VIRTGPU_GETPARAM`（§4.1）——最便宜，且是所有后续尝试的前提
3. XWayland 呈现（§7）——独立、可并行、解锁所有 X11 应用
4. `VIRTGPU_GET_CAPS` + capset 核对（§4.2）
5. 3D 资源 backing（§4.3）——真正的硬骨头
6. `VIRTGPU_EXECBUFFER`（§4.4）
7. 放开 `A20_RENDERER=gl`，合成器切 GL 渲染器
8. retire `A20_GPU_IOCTL_*`（§4.5）
