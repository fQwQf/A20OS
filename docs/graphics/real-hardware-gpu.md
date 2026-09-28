# 真机 GPU：scanout 与 3D 加速是两件事

本文回答一个问题：**A20OS 上"支持真实设备"到底意味着什么工作。** 结论先行：
它不是一个项目，而是**两个数量级不同的项目**。把它们混在一起排期，是真机 GPU
立项时最容易犯、也最贵的错误。

> 本文中的驱动规模是**对 `torvalds/linux` 实测的行数**（`cloc`/`wc -l` 口径，
> 以计数到的源码行为准），用于判断量级，**不是精确工时承诺**。
> 板级 GPU 型号与主线支持状态若标注"未核实"，即表示**无法从一手数据手册确认**，
> 不得当作事实使用。

---

## 0. 两条脊线

| | **Scanout（把像素弄上屏）** | **3D 加速（让 GPU 算）** |
|---|---|---|
| 交付物 | 一块能出画面的显示器 | 可用的 GL/Vulkan 硬件通路 |
| 典型上游规模 | 千到万行 | 十万到数十万行 |
| A20OS 侧工作量 | **2–4 周** | **不可行**（见 §2） |
| 前置 | KMS 对象模型（已有）+ scanout 控制器驱动 | 上述全部 + 用户态驱动栈 + Vulkan/GL 驱动 |

A20OS **已有的**能力恰好是后者的地基：QEMU 里跑着的 llvmpipe + KMS/dumb-buffer
路径（`kernel/drivers/gpu/drm.c`）就是从零 OS 做 3D 的**唯一现实答案**（§4）。
scanout 只需要在上面再加一个上游的显示控制器驱动。

---

## 1. Scanout：2–4 周，按上游驱动大小选

实测的候选驱动规模（`torvalds/linux`）：

| 上游驱动 | 板/控制器 | 规模 | 备注 |
|---|---|---|---|
| `drivers/gpu/drm/sysfb/simpledrm` | 任何 linear framebuffer 设备 | **972 行** | 最省事的起点：有 `simple-framebuffer`/`simplefb` 节点就能出画面 |
| `drivers/gpu/drm/udl` | DisplayLink USB | **1553 行** | 但**上限 2048x2048**、首选格式 16bpp |
| `drivers/gpu/drm/verisilicon` | **DC8200 / DC8000** 显示控制器 | **2207 行** | **VisionFive 2 的 scanout 控制器正是 DC8200**，且已在主线 |
| `drivers/gpu/drm/ast` | ASPEED BMC 显示 | **8386 行** | 数量级已经开始不舒服 |
| `drivers/gpu/drm/loongson` | 龙芯显示 | **6419 行** | **在 LS2K1000 上已经能工作** |

**结论：VisionFive 2 上做真实 HDMI 输出是可行的**，路线是移植
`drivers/gpu/drm/verisilicon`（DC8200），配合 A20OS 已有的 GEM/KMS 层。
**LS2K1000 已经有显示能力**（走 `drivers/gpu/drm/loongson`），尽管它
**没有 GPU，只有 scanout 控制器**——这一点常被误读。

---

## 2. 3D 加速：不要试图 bring up i915 或 amdgpu

同一份树上的实测规模：

| 驱动 | 行数 | 文件数 |
|---|---|---|
| `drivers/gpu/drm/i915`（Intel） | **423437** | 903 |
| `drivers/gpu/drm/amd/amdgpu` | **383440** | 634 |
| `drivers/gpu/drm/imagination`（PowerVR） | **34366** | — |
| 整个 `drivers/gpu/drm` | **987974** | — |

说直白一点：**在一个从零开始的操作系统上 bring up i915 或 amdgpu 不可行。**
PowerVR 的 `imagination` 相对小，但 3.4 万行仍然远超 scanout 那一档，而且它依赖
上游 firmware 接口、复位/时钟框架、IOMMU 与 DMA-BUF 全套约定。

`i915` 与 `amdgpu` 之所以大，很大一部分是它们要覆盖十年硬件、支持虚拟化
（VFIO/GPU passthrough）、多代内存模型与固件兼容性。这些对 A20OS 一样都不需要，
但**你无法只取其中一部分**——驱动不是可以按需裁剪的库。

---

## 3. 逐 SoC 结论

| SoC / 板 | 3D GPU | 主线驱动 | scanout | A20OS 结论 |
|---|---|---|---|---|
| **StarFive VisionFive 2**（JH7110） | **有**：Imagination **BXE-4-32** | ❌ **不在主线** `img,powervr-rogue.yaml` 绑定里；该绑定列的是 `gx6250`/`ge7800`/`axe-1-16m`/`bxm-4-64`/`bxs-4-64`。StarFive 自己的替代 blob **不可再分发** | ✅ **DC8200**，`drivers/gpu/drm/verisilicon` 已在主线 | **HDMI 输出可行，3D 是死路**（见 §3.1） |
| **T-HEAD TH1520**（Lichee Pi 4A） | IMG **BXM-4-64** | ✅ **在**主线绑定内；TH1520 已有主线跑 **Zink**（OpenGL-on-Vulkan）的演示 | 有 | **现实可行的 RISC-V 3D 目标**，预算按**一个季度**（§3.2） |
| **SpacemiT K3**（SG2044） | IMG **BXM-4-64** | ✅ 在主线绑定内 | 有 | 同上，同一档 GPU |
| **Loongson LS2K1000** | **无 GPU** | — | ✅ `drivers/gpu/drm/loongson`，**已工作** | 只有 scanout |
| **Milk-V Duo**（CV1800B / SG2002） | **无 3D GPU** | — | 有（平桥类控制器） | 只能 scanout |
| **K230** | **无 3D GPU**（只有 2.5D VGLite 向量引擎） | — | 有 | VGLite 不是 3D 加速 |
| **EIC7700 / 7700X** | IMG **A 系列** 3D GPU | ❌ 无主线驱动，且 **A 系列早于 `drm/imagination` 支持的范围** | 有 | 3D 短期无解 |

### 3.1 不可再分发性是一条硬约束，不只是技术问题

VisionFive 2 的 3D（BXE-4-32）之所以对 A20OS 是死路，有两个**互相独立**的原因：

1. **主线内核没有 BXE 支持**（`img,powervr-rogue.yaml` 里没有它）。
2. **StarFive 提供的替代 blob 不可再分发。** A20OS 发行的是**可再分发的镜像**，
   把一个不可再分发的 blob 塞进去与这个承诺直接冲突。

即使有人愿意自己承担许可成本，理由 1 仍然成立：没有上游驱动，就没有别人能复现
的移植依据。**不要在 VF2 上排 3D。**

### 3.2 RISC-V 3D 的现实路径

TH1520 / K3 上的 BXM-4-64 **在**主线绑定内，且 TH1520 已有主线跑 Zink 的演示。
这条路是 `PowerVR 主线驱动 + A20OS 内核基础设施 + 用户态`，工作量按
**一个季度**（不是两周）估。前提仍然是 §4 的用户态栈能跑起来，而它现在**已经能跑**
（stock Alpine 用户态 + Linux ioctl ABI）。

### 3.3 未核实项

- **JH7200 / VisionFive 2 Pro 的 GPU 型号无法从一手数据手册确认。** 不猜。
  需要时查 StarFive 官方 datasheet 或主线 binding 变更记录再补。
- 表格中"上游驱动规模"来自 `torvalds/linux` 的行数统计，随主线演进变化；
  引用时请带上你的 checkout 位置。

---

## 4. 从零 OS 做 3D 的正确答案：llvmpipe + KMS/dumb buffer

这不是退而求其次。**对 A20OS 这是正确答案**，理由：

- 用户态是 **stock Alpine 的 Mesa**，里面有 llvmpipe（软件 OpenGL/GLES）与 zink。
  A20OS 不需要写任何 GL 实现。
- 内核需要提供的是 **KMS + dumb buffer + 可用的 mmap/共享路径**，让 Mesa 的
  `kms_swrast` / `swrast` 能建 screen，把结果以 `wl_shm` 交给合成器。
  **这正是 A20OS 在 QEMU 里已经有的东西。**
- 已经在跑通：`es2gears_wayland` 拿到 `EGL_VERSION 1.5` 与
  `OpenGL ES 3.2 Mesa 25.2.7`（llvmpipe），见
  [3d-graphics.md §9.11](3d-graphics.md)。

即：**软件渲染 + 正确的呈现路径**，而不是硬件加速。3D 游戏当前的门是呈现与
性能，不是 GPU（见 [gpu-3d-roadmap.md §0](gpu-3d-roadmap.md) 与 §2）。

**必须硬件加速时**，唯一合理的形态是 **Zink 叠在一个上游 Vulkan 驱动之上**，
而不是自己写 GL/Vulkan。

### 4.1 QEMU 上也没有捷径

QEMU 的 **Venus / virtio-vulkan 路径同样依赖 virglrenderer**：按 QEMU 自己的
文档，OpenGL passthrough 需要 virglrenderer **0.8.2+**，Venus 需要 **1.0.0+**。
所以"在 QEMU 里用硬件 Vulkan"这条路同样卡在宿主 renderer 版本上，不是内核问题。

---

## 5. 建议的真机路线

1. **VisionFive 2 / LS2K1000：做 scanout。** 移植上游
   `drivers/gpu/drm/verisilicon`（DC8200）或复用已有的 `drivers/gpu/drm/loongson`。
   预算 2–4 周。llvmpipe 负责 3D，HDMI 负责出画面。
2. **不要在 VF2 上做 3D**（§3.1）。
3. **若确实需要 RISC-V 硬件 3D**，立项目标是 **TH1520 / K3 + BXM-4-64 + Zink**，
   预算一个季度，且必须先有 §4 的呈现路径。
4. **QEMU 上解硬件 3D**，唯一路径是升级宿主 virglrenderer（见
   [host-tools.md](host-tools.md)），不是改内核。

---

## 6. 引用与核实

本文所有事实分三类，请勿混用：

| 类别 | 例子 | 标注 |
|---|---|---|
| 实测 | 驱动行数与文件数 | 本文表格，注明测量对象 `torvalds/linux` |
| 板级事实 | VF2 的 scanout 控制器是 DC8200；LS2K1000 无 GPU | 需引用 datasheet 或上游驱动文档；本文未附具体来源文件，**引用时请自行补一手来源** |
| 推断 | "3D 是 VF2 上的死路" | 由上面两条 + 可再分发性约束推出 |

**本文没有提供 commit hash、日期或运行日志。** 任何"已在某板验证"的表述都
必须附带命令、配置与提交；本文不作此声称。
