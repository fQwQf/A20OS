# Display 与 Framebuffer 驱动

A20OS 的 display 类（`DEV_CLASS_DISPLAY`，`gpu_dev_ops_t`）包含两层：2D 扫描输出与 framebuffer（本文件主题），以及 virtio-gpu 3D (virgl) 透传（[3D 图形加速栈](../../graphics/3d-graphics.md)）。2D 层只承诺扫描输出；3D 层通过 virtio-gpu 的 `SUBMIT_3D` 把 virgl 命令流转发到 host 端 GL 上下文。

virtio-gpu 已按每个 device 独立分配原生 DMA 实例，内嵌 controlq、锁、等待队列和暂存均由该绑定持有；停止失败保留本实例，不影响其他设备。双设备的逻辑注销/重绑和默认 promotion 已有 QEMU 验证。当前 fbdev/DRM devfs 仍是默认聚合入口，内部实例化不等于多 GPU 节点、跨 GPU PRIME 或显示拓扑管理，详见 [每设备实例](../guide/native-runtime.md#virtio-gpu-的每设备实例)。

## 驱动接入

遵循[共同生命周期](../guide/core-model.md#生命周期的精确语义)，完整同步/异步骨架分别集中在核心模型和[请求队列指南](../guide/request-queues.md#接入骨架)。本页不另设一份手动 free 的 get_fb 退出模板。

| 阶段 | Display provider 的工作 |
|---|---|
| probe 准备 | 选择 managed failed-probe 清理，建立部分实例；先登记 execution stop/native queue，再使硬件访问 ring/backing |
| 初始化与登记 | 取得映射、backing 和模式，准备缓存角色；在本次 probe 内调用 gpu_device_register，最后返回成功，由核心发布 class |
| consumer 调用 | fbdev 每次操作捕获默认 publication 的 access；DRM open 固定 publication，GEM/CRTC/EDID/事件按其分区 |
| execution stop | 在 call drain 前关闭刷新/提交生产者，确认硬件、IRQ/后台工作已停并安全退休请求；失败保留实例与全部硬件可达存储 |
| remove | call drain 后 gpu_device_unregister，交还 producer 的 VMO/queue 等引用及未托管资源；成功后由核心回收 devres |

RAM framebuffer 使用 owning scanout。旧 get_fb 返回借用物理视图，当前没有通用 raw VMA 撤销 helper；不能因 class offline 就 free backing。失败 probe 和 publication 失败走同一可失败退出，不能先 free_fb 再等待硬件停止。

## 原生能力声明

get_capabilities 是纯缓存查询，准备在 gpu_device_register 之前；不访问寄存器、不分配或等待。

| 角色 | 可以声明的条件 |
|---|---|
| SCANOUT | 已实现可用的模式/输出与刷新 |
| RAM_SCANOUT | 已实现符合 get_scanout 契约的 owning RAM 视图 |
| RENDER | 渲染后端已初始化，且相应操作实际实现 |
| BOOT_SCANOUT | 保留固件提供的输出，非完整原生输出接管 |

默认只选择 SCANOUT，先比较 RENDER，同级优先非 BOOT_SCANOUT，等分保持。headless RENDER 可发布但不占默认。consumer 在有效 access 内查询角色；功能位不授予 DMA、用户权限、backing 或固件存储接管权。现有 GPU 表的能力扩展引入于 ABI 12，包须按当前 A20_DRIVER_ABI 重建，不能把旧 vtable 前缀当完整布局。

DRM 文件不跟随默认 promotion 或重绑；跨 publication PRIME 拒绝。per-file handle/FB owner、resident RAM backing 和失败清理保留已实现；完整 master/auth、共享访问权、厂商 BO/GPUVM 及多 GPU 用户节点仍有明确缺口。新原生 GPU 后端以独立协议组合通用 queue/VMO/DMA/fence，virgl 回调不是所有 GPU 的硬件提交标准。

## 类操作

`get_info` 返回当前可见宽、高和 bpp，输出指针无效返回 `-EINVAL`。`get_fb` 只返回借用的 CPU 物理地址和可见 backing 长度，不是完整 VRAM aperture，也不保留存储。只有映射层需要物理地址，驱动的寄存器访问继续使用内核虚拟 MMIO 地址。

`flush(x, y, width, height)` 提交矩形。当前实现把零宽或零高解释为全屏。先验证起点，再用减法裁剪，避免 `x + width` 溢出：

```c
if (x >= fb_width || y >= fb_height)
    return -EINVAL;
if (width > fb_width - x)
    width = fb_width - x;
```

如果设备从普通 RAM backing 读取像素，提交命令前对覆盖区域执行 DMA for-device 同步。stride 可能大于 `width * bytes_per_pixel`，同步长度和用户 ABI 的 `line_length` 都必须使用实际 pitch。

## RAM scanout 的原生所有权

ABI 11 在 GPU vtable 末尾增加可选 `get_scanout`。RAM producer 用 `vmo_create_contiguous()` 创建 backing，记录一份 owner；在有效 class 调用内与模式替换协调，vmo_ref 后返回 `struct gpu_scanout {memory, offset, size}`。extent 和 slot 固定，像素内容可写；offset 页对齐、可见 size 为正，整页覆盖必须落在 backing 内。producer 退出只交还自己的 VMO 引用，不能原阶 pfa_free。只有确认硬件不再访问后才允许释放其硬件 owner。

消费者用 `gpu_device_scanout_get(&access, &snapshot)`，成功后可结束 class access；VMA 或调用者保留 RAM，最后 `gpu_scanout_put()`。helper 验证类型/范围并清理错误输出；回调即使失败，也交出非空 memory 的一份引用。缺少回调才选旧物理分支，已有回调失败则返回错误。最终 put 只依赖内核 MM，不调用已解绑模块。该接口未覆盖 VRAM、每文件设备身份或硬件停止确认，完整错误与锁契约见 [连续 RAM VMO](../guide/native-runtime.md#连续-ram-vmo-与-owning-scanout)。

## `/dev/fb0` ABI

声明位于 `kernel/include/drivers/gpu/framebuffer.h`：

| ioctl | 行为 |
|---|---|
| `FBIOGET_VSCREENINFO` | 返回完整 Linux `fb_var_screeninfo`（含 `xres_virtual`、`xoffset`、RGB bitfield）|
| `FBIOGET_FSCREENINFO` | 返回完整 Linux `fb_fix_screeninfo`（`smem_start`、`smem_len`、`type=FB_TYPE_PACKED_PIXELS`、`visual=FB_VISUAL_TRUECOLOR`、`line_length`）|
| `FBIOPUT_VSCREENINFO` | 不支持模式设置；请求与当前模式一致时按 no-op 成功返回，否则 `-EINVAL` |
| `FBIO_MAP_FB` | 把 framebuffer 映射到参数指定的用户虚拟地址（A20OS 专有）|
| `FBIO_FLUSH` | 当前提交全屏刷新（A20OS 专有）|

`/dev/fb0` 同时支持标准 `mmap(2)`。MMU 上有 get_scanout 的 RAM 路径由普通 `VM_VMO|VM_SHARED|VM_NATIVE_CAP` VMA 保留 VMO，采用 Normal RAM 属性、支持 fork/split，mprotect 不可超过创建时有效硬件权限。偏移页对齐，覆盖不超过可见长度的页取整范围，末页 padding 已清零。旧 get_fb 分支通过共享 MM/Rust 物理事务安装 `VM_PFNMAP|VM_SHARED|VM_DONTFORK`；CPU 缓存类型按 boot framebuffer 别名策略核准，不能统一假设为 Device 或 Normal。hint 检查整个区间，fixed/NOREPLACE 与回滚复用原生 MM。

FBIO_MAP_FB 同样优先 owning RAM 路径，映射完整 scanout 页覆盖并使用固定 NOREPLACE；旧 get_fb 分支继续借用物理存储。用户地址须非零、页对齐且整段不超过 USER_VA_LIMIT。NOMMU 不建 VMA，保留 fixed info 的直接地址兼容行为。物理地址暴露是现有兼容 ABI，不应复制到其他类。

## VirtIO GPU 范例

`virtio_gpu.c` 为每绑定建立独立原生 DMA 实例和 native command queue，支持 MMIO/PCI transport。probe 完成 reset 读回、feature 协商、controlq、RAM scanout、RESOURCE_CREATE_2D/ATTACH_BACKING/SET_SCANOUT、首次 flush 和 display 登记。模式优先读取 GET_DISPLAY_INFO，失败使用 1024×768×32 回退；这不提供完整动态 modeset。

小命令、大响应、SUBMIT_3D、ATTACH_BACKING 共用 native request/execute 路径。实际 wire controlq 仍一个 active chain；command mutex 保护 staging，运行时 IRQ-assisted park 与启动有界 polling 驱动同一后端。请求保留实例 allocation；超时或异常 used id/index 封闭暂存区，成功 reset 前不扩容、复用或退休未知请求。

execution stop 在 class drain 前设 closing、非阻塞检查 active issuer、确认 status=0 并排空 IRQ；忙或 reset 失败返回错误，保留绑定和 pending request。停止成功后队列安全退休，再由 remove 注销默认选择并交还 producer owner；VMA/snapshot 可独立保留旧 RAM。双设备独立 flush、逻辑注销/重绑及旧 VMA 保留已有 QEMU 验证；不等于多 GPU 节点或任意物理拔除保证。

动态模式设置必须先实现 display info 查询和 prepare/commit/rollback，不能只部分实现 `FBIOPUT_VSCREENINFO`。

### VirtIO GPU 3D (virgl)

当 QEMU 以 `virtio-gpu-gl-*` 设备（带 `-display egl-headless` 等 OpenGL 后端）启动时，驱动协商 `VIRTIO_GPU_F_VIRGL`，并可通过 `DRM_IOCTL_VIRTGPU_*` 把 virgl 命令流转发给 host 端 GL 上下文（`CTX_CREATE`/`RESOURCE_CREATE_3D`/`SUBMIT_3D`）。接口、实现与用户态对接见 [3D 图形加速栈](../../graphics/3d-graphics.md)。2D-only 设备自动回退，`gpu3d_test` 读 `VIRTGPU_PARAM_3D_FEATURES` 发现未协商到 virgl 时以退出码 77 报告 `SKIP 2D-only device`。

## VMSVGA/SVGAv3 范例

`vmsvga.c` 匹配 VMware `15ad:0405` 与 VirtualBox ARM SVGAv3 `15ad:0406`。PCI BAR 必须按物理 BAR 号取：BAR0 寄存器，BAR2（部分设备退回 BAR1）VRAM。SVGAv3 从设备读取 `FB_OFFSET`、pitch 和 `FB_SIZE`，只把可见 scanout 暴露给用户态，不能把整段 VRAM 当 framebuffer。

SVGAv3 先启动 device command context，再设置模式；flush 提交 `SVGA3_CMD_UPDATE`。VRAM 在 VirtualBox ARM 映射为 Device memory，因此不要对该 MMIO aperture 使用普通 RAM DMA cache API。remove 关闭 display enable、注销默认设备并清实例。

## 新 display 驱动检查

- 验证 format、bpp、pitch、offset 和 `pitch * height` 的溢出及 aperture 边界。
- 不假定物理 framebuffer 从 BAR 起点开始。
- probe 的自检不得破坏后续用户画面，且失败时撤销类注册。
- `get_fb` 返回的长度必须恰好覆盖用户可写 scanout。
- flush 序列必须符合设备 cache/doorbell 协议。
- 显存映射属性不能与内核已有别名冲突。

不要把整个 PCI BAR 或整段 VRAM 当成 framebuffer 长度返回。`get_fb` 的长度必须只覆盖可见 scanout，否则用户态会映射到未定义或受保护的设备内存。
