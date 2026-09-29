# A20OS 3D 路线图：让 stock Mesa 挂上来，把像素送上屏

3D 能力的配套阅读：[3d-graphics.md](3d-graphics.md)（原理与内核接口细节）、
[real-hardware-gpu.md](real-hardware-gpu.md)（真机 GPU 的现实边界）、
[host-tools.md](host-tools.md)（三个宿主侧工具的用法）与
[drivers/classes/display.md](../drivers/classes/display.md)（显示设备模型）。

> 状态结论以 `kernel/drivers/gpu/` 的 HEAD 源码与
> `kernel/include/drivers/gpu/` 的 UAPI 头为准。"已实现"清单在
> [3d-graphics.md §0](3d-graphics.md)。
>
> 每一条"PASS/已验证"都必须同时给出命令、配置、提交；给不出的，
> 一律写作"未验证"。

先纠正标题。旧标题写的是"GPU 3D 路线图"，很容易被读成"开发着色器"。
A20OS 不写着色器编译器。要解决的是两个具体问题：

1. 让 stock Mesa 挂上来：guest 用户态那份原封不动的 Alpine Mesa
   （`/usr/lib/dri/virtio_gpu_dri.so`）能不能 attach 到 A20OS 的
   `/dev/dri/card0`；
2. 把像素送上屏：窗口、GL 客户端、Minecraft 能出现在扫描输出上，
   并且帧率可以接受。

这两件事里面，第一件是 ABI 与对象模型问题，第二件是呈现与性能问题。
原路线图把它们混成"3D"，于是把大量精力投在已经基本完成的传输层上，
而把最便宜、最有价值的两件事（XWayland 绕过、宿主 renderer 升级）压在末尾。

---

## 0. 一条改变全局的前提：guest 的 GLES 3.2 今天就是通的

这是最容易被忽略、也最能改变优先级顺序的事实。记录在
[3d-graphics.md §9.11](3d-graphics.md#911-前提纠正网络是通的并且-wayland-路径上的真实-gl-渲染已跑通)：

```
es2gears_wayland:  EGL_VERSION = 1.5          然后持续运行到测试超时（Terminated）
eglinfo -p wayland: OpenGL core profile renderer: llvmpipe (LLVM 21.1.2, 128 bits)
                   OpenGL ES profile version: OpenGL ES 3.2 Mesa 25.2.7
```

即 **OpenGL / OpenGL ES 在 guest 应用程序里已经可用**，走的是 llvmpipe +
Wayland（wl_shm）这条路，并且它能持续运行。

由此推出两条必须写进路线图的结论：

- 3D 游戏不是被 GPU 工作卡住的，卡住的是呈现和性能。
  `WLR_RENDERER=pixman`、XWayland 窗口不呈现、窗口尺寸 1024x768 对 854x480、
  llvmpipe 在 TCG 下的帧率，这些是呈现/性能问题，不是 virgl 问题。
- "3D 能跑"与"3D 够快"是两个里程碑，工作内容完全不同。混淆这两者是
  旧路线图误导性的根源。下文 §2 把它们显式分开。

> 一个必须说清的前提：仓库里没有 vendored 的 Mesa、libdrm、EGL、GBM、
> wlroots、labwc、Xwayland、XFCE 源码。整条用户态栈是 stock Alpine apk world
> （`packages/world/xfce.world`），跑在 Linux syscall ABI
> （`kernel/abi/linux/syscall_table.def`）之上，通过 Linux ioctl syscall 访问
> `/dev/dri`。A20OS 内核只贡献 DRM/KMS + virtio-gpu 实现和一层配置 overlay。
> 这是估算任何一项 GPU 工作量时最吃重的一条事实。详见 §3.1。

---

## 1. 一个必须先纠正的前提：目标 UAPI 是 VIRTGPU，不是 VIRGL

社区里讨论 virtio-gpu 3D 时容易说"实现 `DRM_IOCTL_VIRGL_*`"。对现代 Mesa
这是错的，会直接把工作引向一条死路。

Mesa 的 `virtio_gpu_dri.so`（Alpine `mesa-dri-gallium` 里的
`/usr/lib/dri/virtio_gpu_dri.so`）走的是 `virtgpu_drm.h` 里的
`DRM_IOCTL_VIRTGPU_*` 家族，命令号落在 `DRM_COMMAND_BASE`（0x40）之后的
0x41–0x4b，类型字母仍是 `'d'`：

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
的 virtio-gpu 驱动不再使用。

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

### 1.1 A20OS 内核是"运输层"，不解析命令流

这一点决定了工作量的边界，而且它是对的，保留。分工是：

| 环节 | 由谁完成 |
|---|---|
| GLSL → SPIR-V | Mesa（guest 用户态） |
| SPIR-V → virgl 命令流 | Mesa |
| 命令流字节 → host virglrenderer | A20OS 内核（原样透传） |
| virglrenderer → host GPU 执行 | QEMU + virglrenderer（宿主机） |

所以 A20OS 不需要实现 GLSL 编译器、SPIR-V 工具链、光栅化器，也不需要
知道 virgl 命令流的字节格式——那些字节由 Mesa 生成，内核只搬运。内核需要做
对的只有三件事：

1. 暴露 `DRM_IOCTL_VIRTGPU_*`，让 Mesa 能探测能力、建 context、建资源、提交；
2. 3D 资源要有 backing（`RESOURCE_ATTACH_BACKING`），否则 host 侧资源
   无内存可读写，任何渲染都是空转；
3. GEM 对象模型与 KMS 对象模型正确（均已落地，见 §4），这是 GBM 与
   VIRTGPU 两条路径的共同地基。

"开发着色器"在 A20OS 的正确含义是**建运输管道**，不是建编译器。

---

## 2. 两个不同的里程碑：3D 能跑 ≠ 3D 够快

旧路线图只有一个"3D"目标，于是每次硬件相关的能力都被当成"3D 进度"。
实际上要分成：

### 里程碑 A：GL 能用（**已完成**）

guest 里的 GL/GLES 客户端已经在 llvmpipe 上拿到 GLES 3.2 并持续渲染
（§0）。这与 virgl 无关，也与 DRM 传输层无关。不要再为这个里程碑排 GPU 工作。

### 里程碑 B：呈现正确

窗口能出现在屏上。三个已知问题，都在软件/配置层：

- `WLR_RENDERER=pixman`（`A20_RENDERER` 默认值）——合成器不走 GL；
- XWayland 窗口不被呈现（labwc 的 xwm 集成问题，见
  [3d-graphics.md §9.22](3d-graphics.md#922-决定性对照x11xwayland的窗口在这台桌面上根本不出现)）；
- 假 connector 是 1024x768，而 GLFW 只拒绝非正尺寸，所以不是硬阻塞。

里程碑 B 的成本以天计，收益是"所有 X11 应用 + 全部 GL 客户端"。

### 里程碑 C：3D 有性能

把渲染从 CPU 搬到 GPU。两条路：

- C1（virgl）：host virglrenderer 渲染。需要一个够新的宿主 renderer
  （§5），本机**尚未具备**，且这是环境动作。
- C2（llvmpipe + KMS/dumb buffer）：软件渲染但走对通路。这条路 A20OS 在
  QEMU 里已经有了，缺的是合成器侧的呈现（里程碑 B）。真机上的对应物就是
  [real-hardware-gpu.md](real-hardware-gpu.md) 给出的答案：llvmpipe 配一个
  KMS/dumb-buffer 驱动。

结论：**3D 游戏当前的门是里程碑 B，不是 C**。任何把"给 GLES 3.2 加上 virgl"
当成解锁游戏的论证都跳过了这一点。

---

## 3. 用户态不是我们写的（估算工作量的前提）

### 3.1 整条栈是 stock Alpine

| 组件 | 来自哪里 |
|---|---|
| Mesa、libdrm、EGL、GBM、virglrenderer | stock apk（`packages/world/xfce.world`） |
| wlroots、labwc、Xwayland、XFCE | stock apk |
| DRM/KMS + virtio-gpu | A20OS 内核（`kernel/drivers/gpu/`） |
| 访问路径 | Linux ioctl syscall（`kernel/abi/linux/syscall_table.def`） |

推论：

- 内核对的是一个它无法修改的上游。所以 ABI 正确性不是"风格问题"，
  是"能不能挂上"的唯一门槛（§6）。
- 我们不能靠改 Mesa 绕开内核 bug，只能把内核做到 Mesa 期待的样子。
- 反过来，内核侧的任何"我们觉得更合理"的自由发挥都会让 stock Mesa 拒绝挂载。

### 3.2 两个常被引错的"用户态定义"

- `user/external/mlibc/sysdeps/managarm/generic/drm.cpp` **不在 A20OS 构建路径里**。
  A20OS 用 `tools/targets-mlibc.mk` 配置 mlibc 的 `sysdeps/a20`，而
  `sysdeps/a20` 不含任何 DRM 代码。任何把它当"用户态 DRM 定义来源"的引用
  都是无效的（例如 `docs/distro/known-issues.md` 曾用它来否证 ABI 不一致假设；
  结论侥幸成立，因为真正的用户态是 Alpine 的 libdrm，但依据是错的，已更正）。
- `user/cmds/core/egl_test.c` 被 `user/Makefile` 从
  `LOCAL_CMD_SRCS` 里 filter 掉，**不参与构建，是死代码**。不要把它读成
  "A20OS 有 EGL 自测能力"。

---

## 4. 已完成：DRM/KMS 对象模型与 GEM（实测基线）

### 4.1 内核侧实际实现（按代码清点）

`kernel/drivers/gpu/drm.c` 里共 49 个 DRM ioctl：
7 个 device/auth、5 个 GEM、2 个 PRIME、25 个 KMS、10 个 VIRTGPU，
另加一个 16 槽的 A20 私有号段。

KMS 对象是硬编码的单例：1 个 CRTC、1 个 connector、1 个 encoder、1 个 plane，
id 固定。3 个 property、2 个 blob。GEM 对象上限 64，framebuffer 上限 64。

> 也就是说，"让 wlroots/Mesa 满意"这件事在对象数量上永远是够的；
> 真正的风险全在 ABI 精确度上（§6）。

### 4.2 本轮落地的内核修复

以下都改在 `kernel/drivers/gpu/drm.c`（`virtio_gpu.c` 除外处已注明）：

- CRTC 此前根本不存 framebuffer 绑定。`SETCRTC` 呈现成功并返回 0，
  什么都不保存，于是 `GETCRTC` 永远报 `fb_id 0`、`GETPLANE` 硬编码 0。
  现在有 `g_crtc` 结构：`GETCRTC` 报出绑定并回写 connector 列表，primary
  plane 镜像这唯一一份绑定。`mode_valid` 在 `fb_id == 0` 时故意保持 1；
  `mode_valid` 的含义是"CRTC/connector 这一对已经编程了一个 mode"，不是
  "已绑定 framebuffer"；报 0 会让 wlroots 在 backend init 阶段直接放弃这个
  output。
- `PAGE_FLIP` 曾把 `pf.fb_id` 当 GEM handle 解析。framebuffer id 与 GEM
  handle 来自两个独立计数器，之前只是碰巧相等才工作。
- `GETFB2` 曾是一个把结构体清零就返回成功的 stub，于是任何真实
  framebuffer 的查询都像是在查"id 为 0、无几何信息"的 framebuffer。现在从
  后备 GEM 应答。
- `EXECBUFFER` 曾忽略 `bo_handles`：命令流里点名的纹理/顶点缓冲没被映射，
  送到 host 时无内存，渲染进虚空。命令流携带的是 handle 而不是资源 id，
  内核是唯一能解析它们的地方，所以现在由内核在提交前发布缺失的 backing。
  `VIRTGPU_WAIT` 此前不校验任何东西，现在校验 handle。
- 3D 资源生命周期。用户态丢掉 handle 后，host 仍把结果直接写进那些帧，
  所以资源实际上还被映射着；而先释放 VMO 会把它还给分配器，此时 host 仍可能
  在写。`drm_gem_reclaim` 现在先丢资源。
- `DRM_CAP` 有四个能力位此前不在 switch 里，靠 fallthrough 意外返回 0：
  `CURSOR_WIDTH`、`CURSOR_HEIGHT`、`ADDFB2_MODIFIERS`、`PAGE_FLIP_TARGET`。
  现在显式应答，值仍为 0，并写明理由。
- `virtio_gpu.c`：`VIRTIO_GPU_F_CONTEXT_INIT` 此前只从设备位读进来、没有
  回显进 `driver_lo`，于是 host 从未认为它被协商，而 `GETPARAM` 却对外宣称
  `CONTEXT_INIT=1`。现在真正协商，并且 `inst->` 的 flags 改为从 `driver_lo`
  派生。`VIRTIO_GPU_CMD_GET_DISPLAY_INFO` 此前定义了却从未发出，于是 guest
  永远广告一个 1024x768，与宿主窗口无关；现在去查询，零值/无头回报
  时才回落到旧值。

### 4.3 VIRTGPU 传输层已可双向验证

`tools/a20 test smoke-gpu3d-riscv64` 的实测输出：

```
GPU3D_TEST: virgl device present
GPU3D_TEST: transport ctx created / resource created / teardown clean
GPU3D_TEST: GETPARAM 3D_FEATURES=1
GPU3D_TEST: GETPARAM capset mask=0x7 (virgl present)
GPU3D_TEST: GEM handle 1 allocated (16384 bytes)
GPU3D_TEST: 3D resource 2 created with host backing
GPU3D_TEST: RESOURCE_INFO round-trip ok (res 2, size 16384)
GPU3D_TEST: EXECBUFFER accepted a 16 byte stream
GPU3D_TEST: PASS (UAPI surface works; rendering still unproven)
```

反向用例（`GPU_3D=0`）门禁确实 **FAIL**（点名五条缺失 pattern），
所以这不是一个空转的门禁。**这项 PASS 覆盖的是传输可达性与 UAPI 表面，
不覆盖渲染语义**（见 §8）。

backing 的实现要点：

- 3D 资源挂在 GEM handle 上，与 Linux 一致：VIRTGPU 资源由一个 GEM 对象承载，
  内核把该 GEM 的 VMO 页转成 `virtio_gpu_mem_entry[]` 再发
  `RESOURCE_ATTACH_BACKING`。
- 页面必须用 `vmo_get_page_charged()` materialize，不能用
  `vmo_peek_page()`：host 要往这些页里写结果，未分配的页没有帧可写。
- backing 数组是内核构造的物理地址，不能走 `ioctl()` 的
  `copy_from_user` 路径，因此在 `gpu_dev_ops_t` 上新增了 3D 专用 op
  （`resource_attach_backing` / `ctx_create` / `submit_3d` / `get_capset` …），
  把 virtio-gpu 细节留在 `virtio_gpu.c`，GEM 语义留在 `drm.c`。
- virgl context 按 per-open 模型惰性创建（`drm_virtgpu_ensure_ctx()`），
  与 Linux 的 `virtgpu_fprivs` 一致；context id 0 不可用（协议保留）。

### 4.4 一个容易踩的坑：`DRM_CAP_*` 有两套编号

`DRM_IOCTL_GET_CAP` 用的是 libdrm 的 legacy 编号，它与内核
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

`drm_get_cap()` 现有的表是正确的（已按 libdrm 头逐条核对）。若有人"顺手"
按 uapi 顺序重排，PRIME 与 dumb buffer 发现会被静默关闭，且不会有任何报错。

---

## 5. 宿主前置条件：virglrenderer 需要离屏 desktop GL context

这不是 A20OS 的 bug，但会让人误判成 bug，因此必须记下来。

宿主（本机实测）：QEMU 10.0.13、`/dev/kvm` 存在、同时提供 `virtio-gpu-gl-pci`
与 `virtio-gpu-gl-device`、两块 GPU（`renderD128` = AMD Radeon 780M，
`renderD129` = NVIDIA RTX 4060）。virglrenderer 已自建为 1.3.0（见 §5.1）。

```bash
make ARCH=riscv64 GPU_3D=1 QEMU_MEMORY=2G run-world-gui
make ARCH=x86_64  GPU_3D=1 QEMU_MEMORY=4G run-world-gui
```

### 5.0 这里有两层独立的问题，不要混为一谈

第一层（已解决）：QEMU 根本不给 guest `VIRTIO_GPU_F_VIRGL`。

根因是 display backend，不是 renderer 版本。QEMU 的 `egl-headless` display 会
在 render node 上初始化 EGL，而**本机这条 GBM/设备平台路径是坏的**：

```
qemu: egl: eglInitialize failed: EGL_NOT_INITIALIZED
qemu: egl: render node init failed
```

（对照：同一时刻 `eglinfo -p surfaceless` 是成功的，给出
`AMD Radeon 780M (radeonsi, phoenix, LLVM 19.1.7)` / OpenGL 4.6；只有
`eglinfo -p gbm` 拿不到 display。所以坏的不是 EGL 整体，是 GBM/设备平台。）

QEMU 对这个失败不报错，而是**静默交给 guest 一个 2D-only 设备**。这正是本项目
此前多轮"3D 跑不起来"却看不出原因的地方。改用 GLX 后端即可绕开，因为本机
GLX 工作正常（`glxinfo -B` → `AMD Radeon 780M (radeonsi, ...)`，OpenGL 4.6，
direct rendering Yes）：

| display | 现象 |
|---|---|
| `egl-headless`（本项目此前默认） | QEMU 静默降级 → guest `2D only (no VIRGL feature)` |
| `gtk` | `The display backend does not have OpenGL support enabled`，设备起不来 |
| `gtk,gl=on` | ✅ guest 报 `virtio-gpu 3D (virgl)`，`VIRTGPU_PARAM_3D_FEATURES=1` |

`instances/smoke-gpu3d-riscv64.toml` 已改用 `gtk,gl=on`。代价：门禁因此需要
一个 X display（`gtk,gl=on` 要 X），这对 CI 是真实成本，已写在实例注释里。

第二层（未解决）：`GET_CAPS` 仍回 `0x1205`。

```
[GPU] get_capset: resp=0x1205 want=0x1103 | sent ctx=1 idx=0 ver=1
                    | host idx=0 -> id=1 ver=1 size=308 (rc=0)
```

`0x1205` = `VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER`，且我们发的参数与 host 自己
advertise 的完全一致，所以不是请求错，而是 host 在 `virgl_renderer_get_capset()`
内部失败；它需要一个**离屏 desktop GL context**。**换成 1.3.0 之后仍然如此**，
所以这不是 renderer 版本问题。

### 5.0.1 更正一个被当成判据的假信号：`capset size=308`

此前文档把 `size=308` 当作"renderer 是 2020 老版本"的决定性线索（理由是
"现代 capset 是数 KB"）。**这个判据是错的**，现已实测推翻：

- 换成自建的 1.3.0 之后，guest 读到的仍然是 `capset[0] id=1 ver=1 size=308`。

即 capset 1（VIRGL）本身就只有 308 字节，与 renderer 新旧无关。真正能证明
"新库是否生效"的判据是**加载了哪个文件**，不是 capset 大小：

```bash
LD_DEBUG=libs qemu-system-riscv64 ... 2>&1 | grep "trying file=.*libvirglrenderer"
# 期望： trying file=<repo>/tools/virgl/install/lib/x86_64-linux-gnu/libvirglrenderer.so.1
```

这条实测通过（`522e8d9d`）。下一次有人看到 308 不要再去重建 renderer。

### 5.1 解锁工具：`tools/build-virglrenderer.sh`（**已运行，renderer 已升级**）

脚本把 virglrenderer 装到 `tools/virgl/install`，Makefile 在该前缀存在时自动通过
`LD_LIBRARY_PATH` 拾取（`QEMU_LIBPATH`）。用法：

```bash
tools/build-virglrenderer.sh check    # 报告宿主 QEMU、两版 renderer、/dev/dri/renderD128 可访问性
tools/build-virglrenderer.sh build    # 需要 root
tools/build-virglrenderer.sh env      # 打印要用的 LD_LIBRARY_PATH
```

已运行（`fca72f5f`）。宿主 renderer 已从发行版的 1.1.0-2（2020）换成自建的
1.3.0。实测：

- 产出 `libvirglrenderer.so.1.11.0` + `virgl_test_server` 于 `tools/virgl/install`；
- 用 `LD_DEBUG=libs` 确认 QEMU 解析到的是这份新库，而不是系统那份 1.1.0；
- QEMU 确实带 virgl（`virtio-gpu-gl-device` 暴露 `blob` / `hostmem` /
  `max_hostmem` 属性，这些只在启用 virgl 时存在）；
- 宿主两卡：`renderD128` = AMD（amdgpu, 1002:15BF），`renderD129` = NVIDIA
  （10DE:28E0）。**注意与直觉相反**，`DRI_PRIME=radeonsi` 因此不是修法。

但"Mesa 能 attach"仍然没有发生：前置条件（renderer 版本）已清除，剩下的是
宿主 EGL 配置，且现在有了 QEMU 自己打印出来的错误文本。

| display backend | EGL vendor | QEMU 行为 | guest 拿到的 feature |
|---|---|---|---|
| `egl-headless`（默认） | NVIDIA | 静默启动，不给 `VIRTIO_GPU_F_VIRGL` | 2D only |
| `gtk` | NVIDIA | `The display backend does not have OpenGL support enabled` | 起不来 |
| `gtk,gl=on` | NVIDIA | 正常启动 | 2D only |
| `egl-headless` | 强制 Mesa（`__EGL_VENDOR_LIBRARY_FILENAMES=50_mesa.json`） | `eglInitialize failed: EGL_NOT_INITIALIZED` / `render node init failed` | 起不来 |

即：**NVIDIA EGL 下 QEMU 静默降级为 2D；换成 Mesa EGL 则显式失败。** 这与本文档
此前记录的"四种组合都失败"是同一现象，但现在有了 QEMU 的原始报错，且多出一条可用
信息：强制 Mesa EGL 时 `eglinfo -p surfaceless` 是成功的
（`AMD Radeon 780M (radeonsi, phoenix, LLVM 19.1.7)`，OpenGL 4.6），
而 `eglinfo -p gbm` 拿不到 display。**问题落在 GBM/设备平台这条路上，不在
surfaceless**。下一步应从"QEMU 的 egl-headless 走的是哪个 platform"入手，
而不是继续试 EGL vendor 组合。

顺带记下写这个脚本时踩到的坑（都已修在脚本里，且每一个的错误信息都指向别处）：
仓库路径是 `virgl/virglrenderer` 而非 `mesa/...`（写错时 GitLab 返回登录页
而不是 404，看起来像鉴权问题）；归档是 `.tar.bz2`（喂给 `tar` 只报"not in gzip
format"）；1.3.0 没有 `x86-asm` 选项、且 `valgrind` 是布尔（meson 把未知 `-D`
当硬错误，configure 探测一分钟后才炸）；1.3.0 的 gallium 需要 `python3-yaml`；
meson 原生安装到 `lib/<triplet>/`，猜 `lib/` 会让 `LD_LIBRARY_PATH` 静默无效，
症状正是"新库没生效"。

细节见 [host-tools.md](host-tools.md)。另记两个排查陷阱（都实际浪费过时间）：

- `LIBGL_ALWAYS_SOFTWARE=1` 与 `EGL_PLATFORM=surfaceless` 互相矛盾，Mesa
  直接拒绝。表现为 QEMU 提前失败，像 unrelated 的新问题。
- 权限不是原因（在本机）：`/dev/dri/renderD128` 可读可写（带 ACL）。
  （另外：别对字符设备做 `head -c1`，会阻塞；用 `test -r/-w` 判权限。）

---

## 6. 本文档最可迁移的一条教训：错的 ioctl 常量是隐形的

旧文档把这些缺陷的症状当成 Mesa 或 libdrm 的问题来推理。
它们全部是 A20OS 侧的常量与结构体错误。

### 6.1 为什么它隐形

`kernel/include/drivers/gpu/drm.h` 把 ioctl 号写成字面十六进制（它们落在
switch 里，不走宏）。于是一个写错的常量不会报错：驱动的 switch 匹配不上，
ioctl 落到 `default` 分支，用户态看到 `EINVAL`/`ENOTTY`。症状指向 Mesa，
而病灶是一个头文件里的一个字符。本仓库的所有这些号都是肉眼核对过的，
而肉眼核对一次都没有发现它们漂移。

### 6.2 已发现并修复的 ABI 缺陷

| 项 | A20OS 原值 | Linux UAPI | 后果 |
|---|---|---|---|
| `MODE_GETFB2` | `0xc04864ca` | `0xc06864ce` | size 字段与命令号都错，`drmModeGetFB2` 恒返回 `EINVAL` |
| `MODE_RMFB` | `_IOW` | `_IOWR` | `drmModeRmFB` 永不匹配 |
| `GEM_OPEN` | `0xc0186410` | `0xc010640b` | 永不匹配 |
| `struct drm_gem_open` | handle 在 offset 8 | handle 在 4，size 在 8 | 每次 `GEM_OPEN` 都从错误字段读 handle |
| "GEM_GET_HANDLE" | 12 字节结构 | Linux 的 `GEM_FLINK` 是 8 字节 | — |
| `GEM_MMAP` | `0xc010640b` | 这是 Linux 的 `GEM_OPEN` | 命名空间撞车：guest 调真 `GEM_OPEN` 被 mmap handler 应答 |

最后一条最严重，因为它不是笔误而是命名空间撞车。它的成因是：

> Linux UAPI 里根本没有 `GEM_CREATE`、`GEM_MMAP`、`GEM_GET_HANDLE`。
> 用户态分配缓冲走 `MODE_CREATE_DUMB`；给导入用的 handle 起名走 `GEM_FLINK`。
> 所以那三个 A20OS 号没有上游对应物可对齐，而 A20OS 当时正是"照 Linux 的
> 样子"猜的号。于是一个私有号落到了真 `GEM_OPEN` 的位置上。

现状：这三个里 `GEM_FLINK` 已作为真 ioctl 实现；仍保留的两个
（`A20-private`）是 0x4710 / 0x4711，落在 A20 私有号段，不与任何 Linux
号冲突。

同时更正一条旧文档的错误结论：旧版本文档称 `MODE_GETFB2` "已加"，因此
"不是空洞"。它是空洞：ioctl 号错了，从来没被命中过。

### 6.3 门禁：`tools/check-drm-abi.sh`

这个门禁做两件事，都必须做：

1. ioctl 号：把 `drm.h` 里的 `DRM_IOCTL_*` 名字抓出来，针对本机安装的
   `/usr/src/linux-headers-*/include/uapi/drm` 编译一个探针，打印上游的值再
   逐条比对。不问编译器就等于重新实现一遍 `_IOWR()`，而结构体大小正是
   这里的关键。一个把某个布局读错的解析器，要么漏掉真 bug，要么"修好"本来
   正确的代码，后者比没有门禁更糟。编译器已经知道答案。
2. 结构体布局：从 `drm.c` 里把结构体定义 lift 出来编译并量 `sizeof` /
   `offsetof`。正确的 ioctl 号配错误的结构体布局照样静默破坏数据：libdrm
   按 UAPI 说的偏移写字段，驱动按自己那份结构体读，两边都"成功"。

实测：48 个号与 Linux UAPI 一致，1 个无上游对应（`MODE_DPMS`，上游同样没有
定义）。**两个方向都实测会失败**：把 `MODE_RMFB` 扰动一下、从
`drm_mode_fb_cmd` 删掉一个字段，都会被抓出来。漂移时 exit 1，通过 exit 0，
没装 Linux 头文件时干净地 skip（不阻塞非 Linux 宿主）。

细节见 [host-tools.md](host-tools.md)。

---

## 7. 明确仍然未做/未验证的事（不要让它们被"已验证"盖住）

| 项 | 状态 | 说明 |
|---|---|---|
| stock Mesa `virtio_gpu_dri.so` 实际 attach | ❌ 未验证 | VIRTGPU UAPI 已就绪，但宿主 renderer 太老（§5）。不得暗示已验证 |
| 命令流语义 | ❌ 未验证 | `EXECBUFFER` 往返只证明字节到了 host，不证明渲染了任何东西。内核按设计不解析命令流。没有像素回读 |
| 像素回读比对 | ❌ 未实现 | 需要逐字段核对 virglrenderer 的 `virgl_hw.h`（命令类型号、`struct virgl_cmd_header`、各命令结构体布局）。不要凭记忆写：写错只会得到静默失败或 host 崩溃，guest 侧无从判断 |
| `GBM` | ❌ 未通 | `gbm_create_device()` 返回 NULL，钉在 Mesa 的 DRI screen 创建（KMS 路径）。已排除十条假设。对 Wayland 客户端不是阻塞项。最有效的下一步是 guest 里 strace，不是再来一轮假设 |
| 真 DMA-BUF | ❌ 未实现 | `PRIME_HANDLE_TO_FD` 仍只是把 VMO 快照进 memfd，导出后再写入不可见。但基座是现成的，比"从零做"的说法轻：memfd 持有一个 `pfn_t *pages` 物理页数组，mmap 直接把这些 PFN 映射进 VMA，所以它本身已经是可跨进程共享的物理内存对象。真正缺的是一个由 GEM 的 VMO 支撑的 fd：DRM 自己的 mmap 是把 handle 编进 offset 后直接 `mm_mmap_vmo`，PRIME 需要的是一个 mmap op 映射 VMO 的新 vfile 类型，外加 fd 生命周期与引用计数接线。跨文件改动，涉及 `fs/vfs` 与 `mm`，仍然单独立项。不要假装做了 |
| `VIRTGPU_RESOURCE_CREATE_BLOB` | ✅ 报 0 是正确答案，不是缺口 | 见下方专条 |
| 窗口化/局部 present | ✅ 本轮已实现 | `drm_present_buffer_at()` 按扫描-out 边界裁剪，只 flush 实际写入的矩形；位置取 CRTC 记录的 x/y（Linux 语义）。严格的 32bpp 与 pitch 要求保留 |
| 私有 `A20_GPU_IOCTL_*` 3D 传输 ABI（0x4700 段） | ✅ 本轮已删除 | 与 VIRTGPU UAPI 逐条重复，且只有上游那套是 Mesa 会说的。`gpu3d_test` 的三态退出码（77=SKIP）改从 `VIRTGPU_PARAM_3D_FEATURES` 读取后得以保留；为让 SKIP 重新可达，该参数与 `CONTEXT_INIT` 一并改为如实报告已协商的 feature 位 |
| 硬件视频解码 | ❌ 不存在，且在 QEMU 里不可能存在 | virtio-gpu 没有编解码引擎，VA-API 不是"未实现"而是不可达。可行的是软件解码：ffplay 是默认处理器且稳定；mpv 在 A20OS 上因每线程状态 bug 崩溃（见 `docs/distro/mpv-luajit-crash.md`）。virgl 通了之后 `vo=gpu` 能跑，但仍是软解 |
| `A20_RENDERER=gl`（合成器 GL 渲染器） | ❌ 未启用 | 依赖 stock Mesa attach，且合成器侧呈现链路另有 §2 里程碑 B 的问题 |

---

### 7.1 为什么 `VIRTGPU_PARAM_RESOURCE_BLOB` 报 0 是正确的

这一条曾被列为"定义了但未分发"的缺口。核对 QEMU 官方 virtio-gpu 文档后，结论相反：
**报 0 是在当前 QEMU 命令行下唯一安全的答案**，补上分发反而会让 Mesa 走进一条死路。

- 不开 blob，virgl 的 OpenGL 透传本来就能用。QEMU 文档对 OpenGL pass-through 的
  宿主要求写的是"Any Linux version compatible with QEMU if not using host blobs
  feature"。
- blob 的作用是把 guest OpenGL 从 4.3 抬到 4.6。文档原文：默认上限 4.3，
  要 4.6 才需要 `hostmem=` 与 `blob=true`。所以它是一个能力等级，不是能否挂载的前提。
- Venus（Vulkan）与 DRM native context 则确实强制要求 blob。文档对二者的宿主要求
  都写着"requires host blob support (hostmem and blob fields)"。也就是说 Vulkan 不是
  "没实现"，而是被宿主配置挡在门外。
- A20OS 的 QEMU 命令行里没有任何 `memory-backend-file` / `vhost-user` / `hostmem=`
  （已核对根 Makefile 与 `tools/targets-*.mk`），因此宿主侧根本没有 blob 内存窗口。
- blob 没有 virtio feature 位，它是设备属性（`hostmem`/`blob`），guest 侧无从探测。
  既然探测不到，报 0 是唯一说得通的选择；报 1 会让 Mesa 以为有 blob 而走
  `RESOURCE_CREATE_BLOB`，然后在 host 上失败。

若要开这条路，代价与收益是明确的：QEMU 侧加 `hostmem=8G,blob=true`，宿主内核要
6.13+（QEMU 文档对用 blob 的 OpenGL 透传要求"Linux 6.13+"），并且 blob 内存要真正
可用还需要 vhost-user / vhost-kernel 的共享 `memory-backend-file`。收益是 virgl 下
OpenGL 4.6，以及解锁 Venus/Vulkan。这是一条需要单独决策的宿主配置改动，不是内核补一个
分发分支的事，因此本轮不做。

## 8. 验证门禁：必须能失败

旧 `gpu3d_test` 有两个致命问题，二者都让它**在任意配置下都是绿的**：

1. 定义了 `A20_GPU_IOCTL_SUBMIT_3D` 却从未调用，只建了个空 16×16 纹理；
2. 2D-only 设备上打印 `skipping 3D path` 后 `return 0`。

规则：**一个"已验证"结论必须指向一个可以失败的门禁；不能失败的检查不构成
验证。**

已落地：

- `gpu3d_test` 退出码三态：`0` = PASS（virgl 可用且每一步传输都被 host 接受）、
  `77` = SKIP（2D-only，autotools 约定，故意与 PASS 不同）、`1` = FAIL。
  PASS 那行同时声明它没有覆盖什么：未提交命令流语义、资源无像素回读，
  所以这只是*传输可达性*检查，不是"渲染成功"的证明。
- 新增 `instances/smoke-gpu3d-riscv64.toml`，并补上它所需的两个能力：
  - `machine.gpu_3d` 以清单方式选择 virgl 设备；
  - `machine.display_mode = "gui"` 把一次启动从 `-nographic` 切到真实显示 +
    按架构的 GUI 设备集（保留 `-serial stdio`，因为门禁靠串口注入命令与匹配日志）。
    这一步是必需的：冒烟门禁走 `_run_impl`（文本模式），而**文本模式根本不挂
    virtio-gpu**；在那儿做 GPU 测试只能是空转。
- 顺带修掉一个门禁自身的 bug：`a20 test` 生成 QEMU 命令行时会静默丢弃
  额外 make 参数（它们只到了 build，没到 launch），于是
  `a20 test <inst> GPU_3D=0` 会"用一个配置编译、跑另一个配置"。

### 8.1 可证伪验证（双向实测）

| 配置 | guest 日志 | 门禁结果 |
|---|---|---|
| `GPU_3D=1` | `[GPU] virtio-gpu 3D (virgl): capset[0] id=1 ver=1 size=308 ctx_init=1`；`GETPARAM 3D_FEATURES=1`；`capset mask=0x7`；`GEM handle 1 allocated (16384 bytes)`；`3D resource 2 created with host backing`；`RESOURCE_INFO round-trip ok`；`EXECBUFFER accepted a 16 byte stream` | PASS |
| `GPU_3D=0` | `[GPU] virtio-gpu 2D only (no VIRGL feature)` → `GPU3D_TEST: SKIP`（exit 77） | FAIL（点名五条缺失 pattern） |

即：**3D 传输通路 + VIRTGPU UAPI 表面（含 backing attach）已验证**，且这个
验证会失败；反向用例确认了门禁不是空转的。

新增的 UAPI 门禁（`tools/check-drm-abi.sh`）同样双向实测会失败，见 §6.3。

仍然不能说的话（因此"3D 可用"这句话要限定在传输层与 UAPI 层）：见 §7。

文档里的 PASS 只在附上产生它的命令、配置与提交时有效。

---

## 9. 明确排序的论证

排序规则，按优先级：

1. 它是不是 GPU 问题？不是的排前面（因为 GPU 问题的解法往往不在我们手里）。
2. 它能不能在本机做？需要 root / 需要换宿主的排后面。
3. 它解锁多少东西？解锁整类应用的排前面。
4. 它会不会被上游已经做完的工作覆盖？会的排后面（我们不做着色器编译器）。

按这个规则得到下面这个顺序。**旧顺序把两项最便宜、最有价值的事埋在末尾，
却把精力放在一个已经基本做完的子系统上**；这是本路线图最需要修正的地方。

| 序 | 事项 | 是不是 GPU 问题 | 需要 root | 解锁什么 | 成本 |
|---|---|---|---|---|---|
| 1 | XWayland 绕过：Minecraft 走 Wayland 后端 | 否 | 否 | 全部 X11 应用 + Minecraft | 极小（已落地） |
| 2 | 宿主 renderer 升级 + 验证 stock Mesa attach | 否（环境） | 是 | 里程碑 C1 | 环境动作 |
| 3 | 命令流语义验证 + 像素回读 | 是 | 否 | "3D 可用"这句话才成立 | 中 |
| 4 | 合成器呈现（`WLR_RENDERER`/windowed present） | 部分 | 否 | 里程碑 B | 中 |
| 5 | 真 DMA-BUF | 是 | 否 | PRIME/跨进程零拷贝 | 中（基座现成，见 §7） |
| 6 | retire `A20_GPU_IOCTL_*` 私有 3D ABI | 已完成 | 否 | 去掉两套 3D ABI | — |
| 7 | `VIRTGPU_RESOURCE_CREATE_BLOB` 分发 | 不必做 | 否 | 报 0 已是正确答案 | —（见 §7.1） |

第 1 项已经落地：`packages/overlay/xfce/usr/local/bin/minecraft` 现在设置
`XDG_SESSION_TYPE=wayland`，并在缺少 `WAYLAND_DISPLAY` 或 `XDG_RUNTIME_DIR`
时大声拒绝启动。理由与依据见 §9.1。

第 2 项之所以排在 GPU 工作之前：它不需要改 A20OS 一行代码，却直接决定
"stock Mesa 能不能挂上"这个整条路线是否存在；在它完成之前，任何关于 VIRTGPU
UAPI 够不够用的判断都只能是推断。

---

## 9.1 为什么 Minecraft 走 Wayland 后端就够了

镜像里带的是 LWJGL 3.3.3（不是 3.3.6），它捆绑 GLFW 3.4.0。

- 没有 `GLFW_PLATFORM` 这个环境变量：3.4.0 里没有，当前的 GLFW 里也没有；
  它只是一个 init hint，而且这个字符串在镜像里那份 `libglfw.so` 中根本不存在。
  Minecraft 也从不调用 `glfwInitHint(GLFW_PLATFORM, ...)`。
- 在 GLFW 3.4.0 的 `_glfwSelectPlatform` 里，两个后端都编进去、hint 留在
  `ANY_PLATFORM` 时，环境是唯一的杠杆：`XDG_SESSION_TYPE=wayland` 加上
  `WAYLAND_DISPLAY` 会在任何探测之前被采纳；否则它退化成"先试 Wayland 再试
  X11"。而只要 `DISPLAY` 有值就落到 X11，也就是 XWayland，其窗口不被 labwc 的
  xwm 呈现（§7 与 [3d-graphics.md §9.22](3d-graphics.md)）。
- 同时已确认：Wayland 后端不绑定 `zwp_linux_dmabuf_v1`、也不绑定
  `wl_drm`，所以 llvmpipe 走 `wl_shm` 就够。假 connector 是 1024x768，而
  Minecraft 默认窗口 854x480，GLFW 只拒绝非正尺寸。

**XWayland 呈现 bug 本身没有修**，它是被绕过的。文档里不要写成"已修"。

---

## 10. QEMU 测试矩阵

| 目标 | 实例 | 备注 |
|---|---|---|
| 内核 3D 传输 | `tools/a20 test smoke-gpu3d-riscv64` | 需要宿主 virgl；已双向验证 |
| DRM UAPI ABI | `tools/check-drm-abi.sh` | 纯宿主检查，不需要 QEMU |
| Mesa 3D 挂载 | `GPU_3D=1` + xfce world | 需完整镜像，最重；目前被宿主 renderer 挡住 |
| 桌面回归 | `GPU_3D=0` + `xfce-*` | 每次 DRM 改动必跑 |
| 引导 | `smoke-riscv64` / `smoke-abi-linux` | 便宜，可频繁跑 |

```bash
tools/a20 test smoke-gpu3d-riscv64              # 期望 PASS
tools/a20 test smoke-gpu3d-riscv64 GPU_3D=0     # 期望 FAIL（反向验证门禁有效）
tools/check-drm-abi.sh                          # 期望 0 退出
```

迭代目标选择：x86_64 + KVM 是唯一快的环境（LWJGL 也只提供
x86_64/aarch64 native，Minecraft 本来就只能在这两个架构上跑）。riscv64 只有
TCG，适合验证 3D 通路，不适合验证性能。

已知约束：`instances/xfce-x86_64.toml` 把 `smp` 钉在 1（注释称 smp>1 在
x86_64 挂起）。这让唯一快的环境失去多核，建议单独立项排查，否则后续所有
性能判断都不可信。

另：宿主资源不足时 `tools/a20 run|debug|test` 现在会等待而不是失败
（`tools/a20_preflight.py`，见 [host-tools.md](host-tools.md)）。

---

## 11. 明确的非目标（QEMU-first 范围内不做）

- 着色器编译器 / SPIR-V 工具链：Mesa 的职责。
- 自研 DRI 驱动：走 VIRTGPU UAPI 让 stock Mesa 挂载，而不是自己实现一份。
- vendored Mesa / libdrm / wlroots / labwc / XFCE：整条用户态栈是 stock
  Alpine（§3.1）。往里塞自己的 fork 会立刻破坏"让 stock Mesa 挂上"这个目标。
- 真机 GPU 驱动（i915 / amdgpu / VC4 / nouveau / Mali）：对着 QEMU-only
  目标是多月工程；对着真机目标仍然是多月工程，见
  [real-hardware-gpu.md](real-hardware-gpu.md) 的实测驱动规模。
- Vulkan ICD：需要 VIRTGPU 全量 + zink/lavapipe，差得更远。
- 硬件视频解码：QEMU virtio-gpu 没有编解码引擎，VA-API 无从谈起。

---

## 12. 建议顺序（与 §9 同一份清单，含 operator-only 标记）

**已完成**（本轮）：

1. `GPU_3D` 开关在 run 路径生效（§4.3 / §5）
2. KMS 对象模型真实化：CRTC 绑定、`GETCRTC` 回写、`GETFB2` 从后备 GEM 应答、
   `PAGE_FLIP` 不再把 fb_id 当 GEM handle（§4.2）
3. `DRM_CAP` 四个缺失能力位显式应答；3D 资源生命周期修正；`EXECBUFFER` 解析
   `bo_handles`（§4.2）
4. `virtio_gpu.c`：`CONTEXT_INIT` 真正协商、`GET_DISPLAY_INFO` 真正查询（§4.2）
5. UAPI ABI 缺陷修复 + `tools/check-drm-abi.sh` 门禁（§6）
6. lwIP raw RX 路径双重释放修复（`kernel/net/socket_inet.c`，见
   [../distro/known-issues.md](../distro/known-issues.md)）
7. Minecraft 切到 Wayland 后端（§9.1）
8. `tools/build-virglrenderer.sh` 与 `tools/a20_preflight.py` 落地（§5.1 / §10）

**接下来，按 §9 的顺序**：

9. `[operator-only，需要 root]` 跑 `tools/build-virglrenderer.sh build`
   （需先装 `libgbm-dev`、`libdrm-dev`、`libudev-dev`、`python3-mako`），
   然后用 `GPU_3D=1` + xfce world 跑一次，看 `virtio_gpu_dri.so` 能否 attach。
   **判据：guest 日志里的 capset 体积从 308 变成数 KB。** 这是判定"VIRTGPU
   UAPI 是否真的够用"的唯一办法，也是当前唯一没做的大项。本机可做，
   不受 A20OS 侧任何限制影响。
10. 合成器呈现（里程碑 B，`WLR_RENDERER` / 窗口化 present）。本机可做，
    解锁所有 GL 客户端，比继续啃 GPU 性价比高。
11. 命令流语义验证 + 像素回读（§7）：需要逐字段核对 virgl 的 `virgl_hw.h`；
    Mesa 挂载后这件事自动发生，优先级低于 9 与 10。
12. 放开 `A20_RENDERER=gl`（依赖 9 与 10）。
15. 真 DMA-BUF（§7）：基座（memfd 的物理页数组）现成，缺的是 VMO 支撑的 fd，
    单独立项。

在第 9 项完成之前，"3D 可用"这句话只对传输层与 UAPI 表面成立；
"GLES 3.2 可用"这句话则已经对 llvmpipe + Wayland 路径成立（§0）。
第 9、10 项都不需要改内核一行代码，建议优先。

---

## 13. 验证记录（命令 + 配置 + 提交）

本文档上文所有运行类结论都产生于分支 `feat/gpu3d-completion`。**这些提交尚未合入
`main`，因此在合并前上面的 hash 在远端解析不到**。这正是 [../README.md](../README.md)
要求运行类结论必须附提交的原因。

| 结论 | 命令 | 配置 | 提交 |
|---|---|---|---|
| 3D 传输通路 PASS | `tools/a20 test smoke-gpu3d-riscv64` | `ARCH=riscv64`，`GPU_3D=1`，`egl-headless` | `e9b52b0f` |
| 3D 门禁可失败（反向） | `tools/a20 test smoke-gpu3d-riscv64 GPU_3D=0` | 同上，`GPU_3D=0` | `e9b52b0f`（期望 非零退出，实测退出 1） |
| 无内核回归 | `tools/a20 test smoke-riscv64` / `tools/a20 test smoke-abi-linux` | `ARCH=riscv64` | `e9b52b0f` |
| 桌面未回归 | `make ARCH=x86_64 BOARD=qemu-virt-x86_64 PKG_WORLD=xfce QEMU_GUI_DISPLAY=egl-headless NR_CPUS=4 QEMU_MEMORY=4G run-world-gui` | x86_64 + KVM，4 GiB world 镜像 | `e9b52b0f`（labwc / xfce4-panel / xfdesktop / xfsettingsd 均起来；`Failed to set CRTC` 0 次、`Failed to create DRM backend` 0 次、`PANIC` 0 次） |
| 三架构可编译 | `make ARCH={riscv64,x86_64,loongarch64} kernel-only` | 三个主线架构 | `e9b52b0f` |
| DRM UAPI 门禁 | `make check-drm-abi` | 宿主装有 `linux-headers` | `54aec502` 引入，`a3a76bf3` 接入 make（48 个号匹配、1 个无上游对应、6 个结构体布局匹配） |
| UAPI 门禁可失败 | 改坏 `MODE_RMFB` / 从 `drm_mode_fb_cmd` 删一个字段 | — | 两个方向实测均退出非零 |
| 新镜像干净 | `e2fsck -fn build/images/xfce-x86_64.img` | 刚构建 | 5 个 pass 全过 |
| 脏镜像被拒绝启动 | 破坏组描述符表后 `tools/a20 run xfce-x86_64` | — | `ed4bf66f`（拒绝并列出错误；`A20_PREFLIGHT_SKIP_FSCK=1` 可放行） |
| 镜像检查不被资源开关关掉 | `A20_PREFLIGHT=0 tools/a20 run xfce-x86_64`（脏镜像） | — | `ef26f1d6`（仍拒绝）；`A20_PREFLIGHT=0 A20_PREFLIGHT_SKIP_FSCK=1` 放行 |
| 局部 present 可用 | 随 gpu3d 门禁与桌面回归覆盖 | `drm_present_buffer_at()` 裁剪到扫描-out | `0cac968c`（门禁 PASS） |
| 私有 3D ABI 已删除且门禁仍可失败 | `tools/a20 test smoke-gpu3d-riscv64` / 同上 `GPU_3D=0` | `ARCH=riscv64` | `62b6b17f`（正向退出 0；反向退出 1，日志为 `virtio-gpu 2D only (no VIRGL feature)` + `SKIP`） |
| drvmod 模块变体仍可编译 | `make ARCH=riscv64 BOARD=qemu-virt-riscv64 kernel-only DRIVER_DEPLOYMENT=generic` | 通用 profile | `62b6b17f` |
| `strace` 可解析进 world | `tools/a20 build xfce-x86_64` | `xfce.world`，Alpine v3.23 | `7164a94d`（安装 `strace 6.17-r0`，包数 519 → 522） |
| pbuf 崩溃成因已判开 | `make ARCH=x86_64 ... NR_CPUS=4 CONFIG_LWIP_MEMP_OVERFLOW_CHECK=1 run-world-gui` | x86_64 + KVM 桌面 | 本轮（canary 选项由 `25402c3e` 引入）。报 `detected mem underflow in pool PBUF_POOL` 而非 `pbuf_free: p->ref > 0` → 成因是 pool 越界写，不是重复释放，详见 known-issues.md |

注意：这一行是本轮唯一推翻了既有文档结论的测量。此前的记录（无论"是真正的
重复释放"还是"两种假设不可区分"）都不成立；`CONFIG_LWIP_MEMP_OVERFLOW_CHECK` 正是
为区分这两者而加的，它做到了。

| 宿主 renderer 已升级 | `tools/build-virglrenderer.sh build` | 自建前缀，QEMU 经 `LD_LIBRARY_PATH` 加载 | `fca72f5f`（产出 1.3.0；`LD_DEBUG=libs` 确认 QEMU 解析到该库） |
| QEMU 带 virgl 编译 | `qemu-system-riscv64 -device virtio-gpu-gl-device,help` | — | `fca72f5f`（暴露 `blob`/`hostmem`/`max_hostmem`，仅启用 virgl 时存在） |

仍未达成：**"stock Mesa `virtio_gpu_dri.so` 能挂上"至今没有任何正面证据**。
renderer 前置条件已清除，但 QEMU 仍不向 guest 提供 `VIRTIO_GPU_F_VIRGL`
（NVIDIA EGL 下静默降级为 2D；强制 Mesa EGL 则 `eglInitialize failed`）。
完整判据表与下一步见 §5.1。


## guest 实测：llvmpipe 可用，virgl attach 失败（已验证）

在 xfce guest 内（virtio-gpu-gl + `gtk,gl=on`，不挂网卡以避开 pbuf panic）运行
`eglinfo`，实测输出：

```
EGL API version: 1.5
EGL version string: 1.5
EGL client APIs: OpenGL OpenGL_ES
OpenGL core profile vendor: Mesa
OpenGL core profile renderer: llvmpipe (LLVM 21.1.2, 128 bits)
OpenGL core profile version: 4.5 (Core Profile) Mesa 25.2.7
```

软件 3D 通路确认可用：OpenGL 4.5 core + GLES，llvmpipe 128-bit。

**同时直接观测到 stock Mesa 正在尝试 attach virgl 并失败**：同一份日志里出现

```
[GPU] get_capset: resp=0x1205 want=0x1103 | sent ctx=1 idx=0 ver=0
  | host idx=0 -> id=1 ver=1 size=308 (rc=0)     (ctx=1..8)
```

即 Mesa 连续新建 8 个 context 反复重试，每次都在 `GET_CAPS` 处拿到 `0x1205`
后回落到 llvmpipe。这条此前只是推断（"Mesa 能否 attach 未证实"），现在是实测。

两个次要但有用的观测：

- guest 内没有 X display（`glxinfo -B` 报 `unable to open display`）。
  guest 只有 Wayland，因此 `glxgears` / `es2_info` 默认走 GLX 会直接失败，
  必须显式 `EGL_PLATFORM=surfaceless`（或 wayland）。这解释了为什么
  「glxgears 跑不起来」并不代表软件渲染坏了。
- 默认 EGL 驱动是 ZINK（Vulkan），在本 guest 里
  `vkCreateInstance failed (VK_ERROR_INCOMPATIBLE_DRIVER)`，导致 dri2 screen
  创建失败、回落 surfaceless。llvmpipe 仍然可用，但这是绕路而非正常路径。


## 更正：compositor 其实是正常的（此前结论是我的测量事故）

上一条提交曾断言「guest 内没有可用的显示服务器」，**该结论错误**，现更正。

起因是我为了塞诊断而修改 `/sbin/init` 时用了 `t[:i] + new` 的写法，
把 init 脚本截断了：canonical 版本有 90 行，末尾是

```
exec runuser -l root -c /usr/lib/a20/start-xfce4-session
```

这行 `exec` 就是整个会话交接。被截断后 dbus/elogind/seatd/udevd 与会话交接
全部消失，于是观察到「没有 wayland socket、`labwc` 出现 0 次」。那是我自己
改坏的产物，不是 A20OS 的缺陷。

恢复 canonical init（`packages/overlay/xfce/sbin/init`，90 行）后实测：

```
Y1_SOCK   dbus-1  dconf  wayland-0  wayland-0.lock
Y2_WD     /run/user/0/wayland-0
Y3_PROC   62 /extra/usr/bin/labwc
```

labwc 正常启动，Wayland socket 存在。

### 当前真实状态

| 能力 | 状态 |
| --- | --- |
| labwc compositor + Wayland socket | 正常，已实测 |
| llvmpipe GL 4.5 / GLES 上下文（`EGL_PLATFORM=surfaceless`） | 可用，已实测 |
| Wayland 上的 EGL/GLES 应用（`es2_info`） | 仍失败（`Hangup`），未解决 |
| `glxgears` / `glxinfo` | 仍失败：guest 内无 X server / Xwayland 未起，GLX 无从建立 |

因此「3D 游戏可跑」目前依然不成立，但原因已收窄：不是没有显示服务器，
而是 (1) Wayland EGL 路径上 `es2_info` 直接 `Hangup`，
(2) 没有 X server 供 GLX 使用。下一步应查 `es2_info` 在
`WAYLAND_DISPLAY=wayland-0` 下崩溃的原因。

> 教训：给 guest 脚本打补丁时用 `t[:i] + new` 替换会静默截断文件。
> 之后所有此类注入都必须校验行数（canonical = 90 行）并只做插入。
