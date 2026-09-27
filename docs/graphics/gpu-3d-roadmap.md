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

## 4. 已完成：DRM_IOCTL_VIRTGPU_* 与 3D backing

原先列为"待完成"的四步已全部落地，实测见 §4.1。

### 4.1 实测结果（`tools/a20 test smoke-gpu3d-riscv64`）

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

关键点：**3D 资源带 backing 被 host 接受了**（`3D resource 2 created with host
backing`）。这正是原先最大的缺口——此前 `RESOURCE_CREATE_3D` 不带
`ATTACH_BACKING`，host 侧资源没有任何内存，命令流提交上去也是空转。

backing 的实现要点（§4.3 的落地形态）：

- 3D 资源挂在 **GEM handle** 上，与 Linux 一致：VIRTGPU 资源由一个 GEM 对象承载，
  内核把该 GEM 的 VMO 页转成 `virtio_gpu_mem_entry[]` 再发
  `RESOURCE_ATTACH_BACKING`。
- 页面必须用 `vmo_get_page_charged()` **materialize**，不能用
  `vmo_peek_page()`：host 要往这些页里写结果，未分配的页没有帧可写。
- backing 数组是内核构造的**物理地址**，不能走 `ioctl()` 的 `copy_from_user`
  路径，因此在 `gpu_dev_ops_t` 上新增了 3D 专用 op
  （`resource_attach_backing` / `ctx_create` / `submit_3d` / `get_capset` …），
  把 virtio-gpu 细节留在 `virtio_gpu.c`，GEM 语义留在 `drm.c`。
- virgl context 按 **per-open** 模型惰性创建（`drm_virtgpu_ensure_ctx()`），
  与 Linux 的 `virtgpu_fprivs` 一致；context id 0 不可用（协议保留）。

### 4.2 宿主前置条件：virglrenderer 需要离屏 desktop GL context

**这不是 A20OS 的 bug，但会让人误判成 bug，因此必须记下来。**

`GET_CAPS` 在本机会失败：

```
[GPU] get_capset: resp=0x1205 want=0x1103 | sent ctx=1 idx=0 ver=1
                    | host idx=0 -> id=1 ver=1 size=308 (rc=0)
```

`0x1205` = `VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER`。注意**我们发的参数与 host
自己 advertise 的完全一致**（idx=0 → id=1 ver=1 size=308），所以这不是请求
错误，而是 host 在 `virgl_renderer_get_capset()` 内部失败——它需要一个
**离屏 desktop GL context**。

本机把 **display backend × EGL vendor** 四种组合全试了一遍，结论一致：

| display | EGL vendor | QEMU 表现 | `GET_CAPS` |
|---|---|---|---|
| `egl-headless` | 默认（NVIDIA） | 正常启动，`GETPARAM` 全对 | ❌ `0x1205` |
| `gtk` | 默认（NVIDIA） | ❌ `The display backend does not have OpenGL support enabled` | 走不到 |
| `gtk,gl=on` | 默认（NVIDIA） | 正常启动，GL 设备挂上 | ❌ `0x1205` |
| `gtk,gl=on` | 强制 Mesa（`50_mesa.json`） | 正常启动 | ❌ `0x1205` |

**关键判据是第三行**：`-display gtk,gl=on` 已经给 display backend 开上了 GL，
QEMU 也确实把 `-device virtio-gpu-gl-device` 挂上了，但 `GET_CAPS` 依然
`0x1205`。所以"EGL vendor 选到 NVIDIA"只是**表层现象**，不是根因——换成 Mesa
的 EGL、给它一个带 GL 的 display backend，都没有用。

真正的根因指向 **virglrenderer 本身太老**：

```
libvirglrenderer1:amd64 1.1.0-2        # Debian 13，2020 年的版本
[GPU] virtio-gpu 3D (virgl): capset[0] id=1 ver=1 size=308
```

`size=308` 是决定性线索：现代 virgl 的 capset 是**数 KB** 量级（里面要描述
shader 能力、格式表、参数上限等），308 字节只能是极老的 renderer。1.1.0
既没有现代 Mesa（25.2）所依赖的 virgl 协议演进，也建不出它需要的离屏
desktop GL context，于是 `virgl_renderer_get_capset()` 失败并回
`ERR_INVALID_PARAMETER`。

**结论**：本机 virglrenderer 1.1.0 太老，无法为 Mesa 25.2 提供可用的 3D 通路。
下列验证在本机**无法进行**，且**与 A20OS 内核无关**：

- `GET_CAPS`（故 `gpu3d_test` 记为 NOTE 而非 FAIL——它反映宿主能力）
- stock Mesa `virtio_gpu_dri.so` 实际挂载
- `GBM` / `A20_RENDERER=gl`
- 像素回读（需要 host 真的执行渲染）

**解锁办法**：装一个现代 virglrenderer（≥ 0.11），或换一台带新 virglrenderer
的宿主。这不是改 A20OS 能解决的。

注意本机 **apt 源里没有更新的版本**（Debian trixie 只有 1.1.0-2）：

```
$ apt-cache policy libvirglrenderer1
已安装：1.1.0-2
候选：  1.1.0-2          # 没有更新版本
```

所以在本机只有两条路：**从源码编译 virglrenderer**（装到 `QEMU_` 前缀，或用
`LD_LIBRARY_PATH` 覆盖），或者**换宿主/发行版**。这是环境动作，需要 root，
不应由自动化代理擅自改动宿主机。

### 4.2.1 两个排查陷阱（都实际浪费过时间）

- `LIBGL_ALWAYS_SOFTWARE=1` 与 `EGL_PLATFORM=surfaceless` **互相矛盾**，Mesa
  直接拒绝：`Not allowed to force software rendering when API explicitly
  selects a hardware device.`。表现为 QEMU 提前失败，像 unrelated 的新问题。
- 权限**不是**原因：`/dev/dri/renderD128` 是 `root:render` 且当前用户不在
  `render` 组，但设备带 ACL，`test -r/-w` 均为 yes，可正常访问。
  （另外：别对字符设备做 `head -c1`，会阻塞——用 `test -r/-w` 判权限。）

### 4.3 仍然未验证：命令流语义

`EXECBUFFER accepted a 16 byte stream` 只证明**往返通了**：命令流送到了 host
并拿到了应答。内核不解析命令流，所以这**不证明渲染了任何东西**。

要让 `gpu3d_test` 真正验证"渲染"，需要提交一条内容符合 virgl 命令编码的
命令流，并回读像素做比对。这要求逐字段核对 virglrenderer 的
`virgl_hw.h`（命令类型号、`struct virgl_cmd_header`、各命令结构体布局），
**不要凭记忆写**——写错只会得到一个静默失败或 host 崩溃，且 guest 侧无从判断。

Mesa 挂载后这件事会自动发生：命令流由 Mesa 生成，A20OS 只搬运。

### 4.4 仍然未做：retire 私有 ABI

`A20_GPU_IOCTL_*`（`virtio_gpu.h:272-277`）是 A20 私有 fork。VIRTGPU UAPI
可用后它就没有存在理由：留着等于维护两套 3D ABI。尚未删除，因为
`gpu3d_test` 的第一层仍在用它做廉价的传输层回归；等 §4.3 的命令流验证补上
之后即可一并 retire，并同步修正 `3d-graphics.md` 的 §2 与 §4。

---

## 5. 验证门禁：必须能失败

旧 `gpu3d_test` 有两个致命问题，二者都让它**在任意配置下都是绿的**：

1. 定义了 `A20_GPU_IOCTL_SUBMIT_3D` 却**从未调用**，只建了个空 16×16 纹理；
2. 2D-only 设备上打印 `skipping 3D path` 后 `return 0`。

规则（本轮确立，替代旧做法）：**一个"已验证"结论必须指向一个可以失败的门禁；
不能失败的检查不构成验证。**

本轮已落地：

- `gpu3d_test` 退出码三态：`0` = PASS（virgl 可用且每一步传输都被 host 接受）、
  `77` = SKIP（2D-only，autotools 约定，**故意**与 PASS 不同）、`1` = FAIL。
  PASS 那行同时声明它**没有**覆盖什么：未提交命令流、资源无 backing，所以这只是
  *传输可达性*检查，不是"渲染成功"的证明。
- 新增 `instances/smoke-gpu3d-riscv64.toml`，并补上它所需的两个能力：
  - `machine.gpu_3d` 以清单方式选择 virgl 设备；
  - `machine.display_mode = "gui"` 把一次启动从 `-nographic` 切到真实显示 +
    按架构的 GUI 设备集（保留 `-serial stdio`，因为门禁靠串口注入命令与匹配日志）。
    这一步是必需的：冒烟门禁走 `_run_impl`（文本模式），而**文本模式根本不挂
    virtio-gpu**——在那儿做 GPU 测试只能是空转。
- 顺带修掉一个门禁自身的 bug：`a20 test` 生成 QEMU 命令行时会**静默丢弃**
  额外 make 参数（它们只到了 build，没到 launch），于是
  `a20 test <inst> GPU_3D=0` 会"用一个配置编译、跑另一个配置"。

### 5.1 已完成的可证伪验证（双向实测）

| 配置 | guest 日志 | 门禁结果 |
|---|---|---|
| `GPU_3D=1` | `[GPU] virtio-gpu 3D (virgl): capset[0] id=1 ver=1 size=308 ctx_init=1`；`GETPARAM 3D_FEATURES=1`；`capset mask=0x7`；`GEM handle 1 allocated (16384 bytes)`；`3D resource 2 created with host backing`；`RESOURCE_INFO round-trip ok`；`EXECBUFFER accepted a 16 byte stream` | **PASS** |
| `GPU_3D=0` | `[GPU] virtio-gpu 2D only (no VIRGL feature)` → `GPU3D_TEST: SKIP`（exit 77） | **FAIL**（点名五条缺失 pattern） |

即：**3D 传输通路 + VIRTGPU UAPI 表面（含 backing attach）已验证**，且这个
验证会失败——反向用例确认了门禁不是空转的。

仍然**不能**说的话（因此"3D 可用"这句话要限定在传输层与 UAPI 层）：

| 未覆盖 | 对应 |
|---|---|
| 命令流的**语义**（host 是否真的执行了渲染） | §4.3 |
| 像素回读比对 | §4.3 |
| stock Mesa 实际挂载（需完整 xfce 镜像） | §8 |
| `GET_CAPS`（本机宿主 EGL 限制，见 §4.2） | §4.2 |

文档里的 PASS 只在附上产生它的命令、配置与提交时有效。

---

## 6. QEMU 测试矩阵

| 目标 | 实例 | 备注 |
|---|---|---|
| 内核 3D 传输 | `tools/a20 test smoke-gpu3d-riscv64` | 需要宿主 virgl；已双向验证 |
| Mesa 3D 挂载 | `GPU_3D=1` + xfce world | 需完整镜像，最重 |
| 桌面回归 | `GPU_3D=0` + `xfce-*` | 每次 DRM 改动必跑 |
| 引导 | `smoke-riscv64` / `smoke-abi-linux` | 便宜，可频繁跑 |

```bash
tools/a20 test smoke-gpu3d-riscv64              # 期望 PASS
tools/a20 test smoke-gpu3d-riscv64 GPU_3D=0     # 期望 FAIL（反向验证门禁有效）
```

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

已完成（本轮）：

1. `GPU_3D` 开关 + `extra_qemu` 在 run 路径生效（§2）
2. GEM 对象模型 + `MODE_GETFB2` + `GEM_CLOSE` 真正释放（§3）
3. `VIRTGPU_GETPARAM` / `GET_CAPS` / `RESOURCE_CREATE`（含 backing attach）/
   `RESOURCE_INFO` / `EXECBUFFER` / `WAIT` / `MAP` / `CONTEXT_INIT` /
   `TRANSFER_*`（§4.1）
4. 可证伪门禁与三态退出码（§5）

接下来：

5. **装一个现代 virglrenderer（≥ 0.11），然后验证 stock Mesa 实际挂载** ——
   用 `GPU_3D=1` + xfce world 跑一次，看 `virtio_gpu_dri.so` 能否 attach。
   这是判定"VIRTGPU UAPI 是否真的够用"的唯一办法，也是本轮唯一没做的大项。
   **前置：第 5、7、8 项在本机全部被宿主挡住**——宿主 `libvirglrenderer1 1.1.0`
   太老，建不出 Mesa 25.2 需要的离屏 desktop GL context；四种
   display×EGL 组合（含 `-display gtk,gl=on` + 强制 Mesa EGL）均失败，
   完整证据与两个排查陷阱见 §4.2。**这不是 A20OS 的问题，改宿主即可解锁。**
6. **XWayland 呈现**（§7）——独立、可并行、解锁所有 X11 应用（含 Minecraft）。
   **本机可做，不受上述宿主限制影响**，性价比高于继续啃 GPU。
7. 命令流语义验证 + 像素回读（§4.3）——需要逐字段核对 virgl 的
   `virgl_hw.h`；Mesa 挂载后这件事自动发生，优先级低于 5 与 6。
8. 放开 `A20_RENDERER=gl`，合成器切 GL 渲染器（依赖 5）。
9. retire `A20_GPU_IOCTL_*`（§4.4）。

在 5 完成之前，"3D 可用"这句话只对**传输层与 UAPI 表面**成立。
第 6 项不受宿主限制，建议优先。
