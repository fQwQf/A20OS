# A20OS 3D 图形加速栈（virtio-gpu / virgl）

阅读前提是理解 [Display/Framebuffer 驱动](../drivers/classes/display.md) 与 [PCI 与 VirtIO](../drivers/guide/pci-and-virtio.md)。

> 状态结论以 `kernel/drivers/gpu/` 的 HEAD 源码为准。任何"已实现/已验证"的判断
> 都必须能指向一个**可以失败**的门禁；不能失败的检查
> 不构成验证（见 §0）。

---

## 0. 当前状态清单

早期版本在开头断言"内核侧 3D 命令路径已可用"，并给出
`GPU3D_TEST: PASS` 作为验收输出。**这两条都不成立**，已按实测更正：

- 曾经的 `gpu3d_test` 只创建 context 与一个空的 16×16 纹理就销毁，
  `A20_GPU_IOCTL_SUBMIT_3D` 定义了却从未被调用，**一个命令流都没提交过**
  （该私有 ioctl 已随 §2.2 一起删除；同一条通路现在由
  `DRM_IOCTL_VIRTGPU_EXECBUFFER` 承担，并被门禁实际调用）；
- 更严重的是它在 2D-only 设备上打印 `skipping 3D path` 后 `return 0`，
  于是**任何配置下这个测试都是绿的**，绿灯不携带任何信息。
- 第三轮把 16 字节全零占位流换成真实的 `VIRGL_CCMD_CLEAR` + 像素回读后，
  同一个占位流曾让门禁报出 `PASS (rendering still unproven)`：
  **"host 接受了字节"被当成了"渲染成功"**。现在这条不匹配会让门禁红。

按能力逐条核对当前状态：

| 能力 | 状态 | 依据 / 缺口 |
|---|---|---|
| virtio-gpu 2D scanout、modeset、page-flip | ✅ 可用 | 桌面长期运行其上，有 QMP 截屏证据 |
| virtio-gpu 3D 协议结构体与命令封装 | ✅ 已实现 | `virtio_gpu.h` / `virtio_gpu.c` 的 `CTX_CREATE`/`RESOURCE_CREATE_3D`/`SUBMIT_3D`/`RESOURCE_UNREF` |
| QEMU 侧提供 virgl 设备 | ✅ **本轮新增** | `GPU_3D=1` 选择 `virtio-gpu-gl-*`；此前所有实例都是 2D-only |
| 3D 传输通路端到端 | ✅ **本轮已双向验证** | `tools/a20 test smoke-gpu3d-riscv64`：guest 协商到 VIRGL 并从 host virglrenderer 读到 `capset[0] id=1 ver=1 size=308`；反向（`GPU_3D=0`）门禁确实 FAIL。前提是 display 用 GLX 后端（`gtk,gl=on`）；`egl-headless` 会让 QEMU 静默降级为 2D-only，见 [gpu-3d-roadmap.md §5.0](gpu-3d-roadmap.md) |
| 3D 资源挂载 backing | ✅ **本轮已实现并验证** | VIRTGPU 资源由 GEM handle 承载，内核把 VMO 页 materialize（`vmo_get_page_charged`）后转成 `virtio_gpu_mem_entry[]` 发 `RESOURCE_ATTACH_BACKING`；实测 host 接受：`3D resource 2 created with host backing` |
| 命令流提交 | ✅ **本轮已验证到像素** | `gpu3d_test` 提交真实 `VIRGL_CCMD_CLEAR`（`CREATE_OBJECT(SURFACE)` → `SET_FRAMEBUFFER_STATE` → `CLEAR`，76 字节），并**回读像素**：两遍颜色都中（红 `0xffff0000`、蓝 `0xff0000ff`，各 4096 像素）。此前"编码正确但像素未验证"的状态是三个缺陷叠加的结果——SUBMIT_3D 命令头多带一个 mem_entry 导致命令体被宿主按固定 32 字节偏移截断、从未下发 `CTX_ATTACH_RESOURCE`、`TRANSFER_FROM_HOST` 是空实现；见 §0.5 |
| 上游 `DRM_IOCTL_VIRTGPU_*` UAPI | ✅ **本轮已实现** | `GETPARAM`/`GET_CAPS`/`RESOURCE_CREATE`/`RESOURCE_INFO`/`EXECBUFFER`/`WAIT`/`MAP`/`CONTEXT_INIT`/`TRANSFER_*`；ioctl 号与 Linux UAPI 逐条比对过（`tools/check-drm-abi.sh`），未与 legacy `DRM_IOCTL_VIRGL_*` 混淆（[gpu-3d-roadmap.md §1](gpu-3d-roadmap.md)） |
| `GET_CAPS` 在本机可用 | ✅ **本轮已实测可用** | guest 日志 `GET_CAPS returned a 308 byte capset`。此前记为"宿主限制"是错的：那次失败是**我们**把 capset *index* 当 *id* 发了（提交 `68b54d32`），修掉后同一台宿主正常返回。**`capset size=308` 也不是「renderer 老」的判据**：GET_CAPSET_INFO 按索引查，index 0 就是 capset 1，而 capset 1 本就小（见 [gpu-3d-roadmap.md §5.0.1](gpu-3d-roadmap.md)） |
| stock Mesa 实际挂载 | ❌ **未验证** | VIRTGPU UAPI 已就绪，但尚未用完整 xfce 镜像跑一次 `virtio_gpu_dri.so` attach 来确认够用。宿主 renderer 已从 1.1.0-2 换成自建的 1.3.0（`tools/build-virglrenderer.sh`，`fca72f5f`），但 QEMU 仍不向 guest 提供 `VIRTIO_GPU_F_VIRGL`：NVIDIA EGL 下静默降级为 2D，强制 Mesa EGL 则 `eglInitialize failed`。因此这条仍未解决，且卡点已收窄到宿主 EGL/GBM 平台选择（见 gpu-3d-roadmap.md §5.1）（[gpu-3d-roadmap.md §5.1](gpu-3d-roadmap.md)） |
| guest 里的 GL/GLES 客户端 | ✅ **已可用（llvmpipe）** | `es2gears_wayland` 在 Wayland 路径上跑到测试超时，`eglinfo -p wayland` 报 `OpenGL ES profile version: OpenGL ES 3.2 Mesa 25.2.7`（llvmpipe，LLVM 21.1.2）。即"3D 游戏"当前被呈现与性能卡住，而不是被 GPU 卡住（[gpu-3d-roadmap.md §0](gpu-3d-roadmap.md)） |
| DRM GEM 对象模型 | ✅ **本轮已实现** | `GEM_OPEN`/`GEM_FLINK` 为真 UAPI ioctl，`GEM_CLOSE` 真正释放，dumb buffer 复用同一分配器，上限 64。`GEM_CREATE`/`GEM_MMAP` 已不是 UAPI 概念，见 §8.1 |
| KMS 对象模型 | ✅ **本轮已实现** | 1 CRTC / 1 connector / 1 encoder / 1 plane，硬编码 id；CRTC 真正保存 framebuffer 绑定，`GETCRTC` 报绑定并回写 connector 列表。framebuffer 上限 64 |
| 真 dma-buf（PRIME） | ❌ 未实现 | 仍然是把 VMO 快照 memcpy 进 memfd，导出后再写入不可见。A20OS 无跨进程 VMO 共享、无 mmap-offset 协议，所以这是从零做而不是打补丁。**本轮修掉的是导出/导入本身的正确性**：映射表原先按 fd *编号* 记录且从不删除，用户态一 close，内核就把该编号复用给别的文件，于是 `PRIME_FD_TO_HANDLE` 会把旧 GEM handle 发给一个不相干的 fd——而 handle 是能经 `drm_linux_mmap()` mmap 的。现在按 `vfile.identity`（单调递增的 per-open id）记录，用户已关闭的条目会被回收，写不进表的 fd 不再发出去。真正共享内存仍然没有 |
| 合成器 GL 渲染器 | ❌ 未启用 | `A20_RENDERER` 默认 `pixman`（会话脚本不再硬编码，但默认值不变） |
| Mesa/virgl 用户态客户端 | ❌ 未挂载验证 | UAPI 已就绪；A20OS 不自建 DRI 驱动，由 stock Mesa 提供（见上一行） |

### 本轮（`feat/graphics-hardening`）：存储层的正确性，而不是新能力

上一轮把 3D 通路打通之后，留在树里的问题已经不是"还差什么功能"，而是
"已经声称做到的事情里，哪些是真的"。逐条核对源码后修掉的都是这一类，
判据仍然是"能不能指向一个会失败的门禁"。

**一处此前完全不存在的锁。**GEM 表、framebuffer 表、CRTC 绑定、GEM name 表、
PRIME 表、virgl id 计数器、EDID 缓存——全部是 device-global，且按设计就是
全局的（一个 open 建的 buffer 必须对另一个 open 可见），而**此前没有任何互斥**，
只有 vblank 那个槽加了锁。这不是理论竞态：桌面天生两个进程并发驱动它
（wlroots allocator 在一个 fd 上建 buffer，backend 在另一个 fd 上用）。
两个 ioctl 可以发出同一个 handle、在另一个 CPU 遍历时删掉表项，或者把第三个
ioctl 正在读的 VMO 释放掉。现在有 `g_drm_store.lock`，但它**只保护表**：凡是
可能睡眠、分配、进 VFS 或下发 virtio 命令的操作都在放下锁之后做，跨越这段的
对象由 pin 兜底，所以 teardown 拆成两段（锁内 `drm_gem_detach_locked()`、
锁外 `drm_gem_drop_storage()`）。契约记在
[lock-order.md](../drivers/guide/lock-order.md#drmkmsvirtio-gpu-3d)。

**一处 use-after-free，在 3D 传输层。**`virtio_gpu_resource_attach_backing()`
在**完全没有持锁**的情况下 resize / `kfree` / `memset` / `memcpy` 共享的
`inst->big_req`，之后才去拿 `inst->command_lock`。而同一块 buffer 也被
`send_cmd_big` 与 `submit_3d` 在同一把锁下增长和重填；DRM 每次 EXECBUFFER
都会为每个 buffer 调一次 ATTACH_BACKING。两个客户端就会互相把对方的请求体
写坏，其中一个还会 `kfree` 掉另一个正在写的内存。

**一处静默画错。**`RESOURCE_ATTACH_BACKING` 的 page 列表原先是"跳过分不到的、
把剩下的压紧"。宿主按 `entries[n]` 放在 `n * PAGE_SIZE`，所以 8 页里第 3 页
失败时，entry 3 变成第 4 页的物理帧，渲染器画进一个从未拿到过的 buffer，
而整条路径报的是成功。现在部分页集直接算失败。

**host 侧资源泄漏两处。**同一个 buffer 被二次提升为 3D resource 时，
`virgl_res_id` 被直接覆盖，旧 resource 永远不 unref；`drm_close()` 一边要求
`ops->ctx_destroy` 存在、一边从不调用它，于是每个碰过 3D 的 `renderD128`
open 都在 host 上漏一个 context，直到 guest 结束。

**尺寸算术。**CREATE_DUMB 的 pitch 是 `((width * bpp + 7) / 8 + 63) & ~63u`，
`width * bpp` 是 32 位乘法。实测旧算法下 `65536 x 65536 x 32` 得到
`size = 17179869184`（16 GiB），CREATE_DUMB 照单全收，PRIME 导出再
`kmalloc(b->size)` 快照一份——两个 ioctl 向内核要 16 GiB VMO 加 16 GiB 堆。
算术已移到 `kernel/include/drivers/gpu/drm_geom.h` 的 `drm_dumb_layout()`，
全 64 位，并以 `DRM_MAX_BUFFER_BYTES` 封顶；这个上限不是凑的 256 MiB，
它正好等于 3D 传输层本来就会拒绝的上限（`attach_backing` 超过 65536 页即拒），
于是 VMO 与导出快照共用同一个界。

**五处"声称做到但没做到"。**`GETPROPBLOB` 在 `b.data` 为空或偏短时**不拷贝却
返回成功**，把调用方未初始化的栈当 EDID 交出去（应 `-ENOSPC` 并回填所需长度）；
`SET_CLIENT_CAP` 接受 `DRM_CLIENT_CAP_ATOMIC` 而 `MODE_ATOMIC` 对一切非
TEST_ONLY 提交返回 `-EINVAL`——桌面只是靠会话里 `WLR_DRM_NO_ATOMIC=1` 才没炸；
`GEM_FLINK` 名字表满时吞掉失败仍返回成功；`ADDFB`/`ADDFB2`/`RESOURCE_CREATE`
在 copy-out 之前就占了表项或 host 资源，copy 失败即永久泄漏（RESOURCE_CREATE
还会留下一个谁也不知道 id 的 virgl resource）；`RMFB` 把仍绑在 CRTC 上的
framebuffer 销毁掉，`GETCRTC`/`GETPLANE` 于是继续报告一个 `GETFB` 已经
`-ENOENT` 的 fb_id。

**一处越权。**render node 此前只拒绝 `SET_MASTER`，其余 KMS ioctl 从
`/dev/dri/renderD128` 一律可达，于是 GBM/EGL 进程可以 `MODE_SETCRTC` 把屏幕
抢过去；而 `is_master` 从未被任何 KMS handler 查询过，所以 master 标志本身
也从来不是保护。[3d-graphics.md §6](3d-graphics.md) 早就写着 render-only
"KMS ioctl 本就需要 master"——现在它真的如此。**注意这是本轮唯一一处会影响
既有桌面的行为改动**，上线前需要跑一次 `GPU_3D=0` 的 xfce smoke。Linux 同样
在 render node 上拒绝 `CREATE_DUMB`，所以 GBM 本来就用 primary node 建 dumb
buffer，没有依赖旧行为的东西。

> **该 smoke 已跑，结果见 §0.6**：2D-only（`virtio-gpu 2D only (no VIRGL
> feature)`）实例 xfce-x86_64 正常起来并反复呈现（`[DRM] present handle=N
> 1024x768`），labwc 认出 `Found config * for output Virtual-1`，全程没有一行
> master/render-node 错误。这条阻塞解除。

**一处会静默掐死整条 3D 路。**`gpu_device_register()` 是 first-wins，而
`vmsvga` 的 ops 表 3D 部分全是 NULL。若它先 probe，所有 `DRM_IOCTL_VIRTGPU_*`
返回 `-ENODEV`，而 `DRM_CAP_PRIME`/`GETPARAM` 照旧声称有能力，日志里没有一行
说 DRM 到底绑了谁。现在按"3D 可用者优先、且同级不互相顶替"选择，两条路径都打日志。

**门禁。**`tools/tests/drm_geom_test.c` 直接驱动 `drm_dumb_layout()`（由既有
`make host-tests` 的通配自动纳入）；`make check-drm-store-locking` 断言锁契约，
并对 `drm_lock()`/`drm_unlock()` 的出现次数设下限——只断言"符号出现过"在删掉
某处锁之后仍然全绿，那样的门禁会放过自己存在的目的。两道门禁都实测过会失败：
把旧算术放回 `drm_dumb_layout()` 测试退出 134；删掉 15 对 lock/unlock 计数转红。

**本轮没做的事，明确记下。**`virtio_gpu_remove()` 与 `init_transport` 的
`fail:` 路径原先直接 `memset` 整个 instance（含 `command_lock` 与 waiters）而不持锁，
会把别人阻塞其中的 mutex 和已挂链的 waiter 一起清掉。**本轮已把这两处换成
`virtio_gpu_release_buffers()`**：释放堆缓冲、清零标量状态，但**不再碰
`command_lock` 与 waiters**（instance 在静态存储里，保留已初始化的锁本身就是对的）。


**曾经记为未解决、现已查清并撤回的一条**：上面这个 helper 的注释曾写"仍在命令
路径里的调用者会继续跑在已 reset 的 transport 上，因此需要设备级 teardown 锁或
in-flight 引用计数"。**这条判断是错的**，查证后撤回，理由是它要防的那件事不会发生：

- instance 是文件静态的 `g_gpu_inst`，其存储寿命长于任何调用者，所以"跑在已
  reset 的 transport 上"不构成 use-after-free；
- 每个命令入口都重新读 `dev->drv_priv`，而 `remove()` 会把它清空，wrapper 在
  NULL 时回 `-ENODEV`；
- ops 表是 `static const`，竞态调用者已经取到的函数指针始终有效；
- 能走到驱动的前提是该设备是当前默认设备，而 `gpu_device_unregister()` 先清空
  槽位，于是 `drm_gpu_ops()` 返回 NULL，ioctl 在碰到 instance 之前就回 `-ENODEV`。

真正让这个顺序安全的是：`unregister()`（清槽位）→ `unpublish()`（排空
`class_device_call_begin` 使用者）→ `release_buffers()`。**若将来有改动在一次
ioctl 期间缓存 `dev` 或 `ops`，或绕过默认设备槽位直接调驱动，这个结论就失效，
那时才真的需要引用计数。**
`gpu_device_unregister()` 的"清空槽位后不重选"本轮也修掉了，见 [§0.6](#06-本轮合并前阻塞项已解除设备重选已修)。

### 本轮（`feat/graphics-hardening`，续）：宿主阻塞解除后的实测结论

前面把 GET_CAPS 归因于"宿主 renderer 建不出离屏 GL context"。**这个结论是错的。**
在一台裸机宿主（AMD + NVIDIA 双 GPU、无 X、无头显）上实测，宿主完全能给出
OpenGL 4.6 context；错的是我们自己把 capset **index** 当成 capset **id** 发给了
GET_CAPSET（见提交 `68b54d32`）。修掉之后 guest 日志变成
`GET_CAPS returned a 308 byte capset`，此前是 0x1205。

顺带更正一条本文档 §0 里的判断：`capset size=308` 既不能证明 renderer 老，也不能
证明 renderer 新。GET_CAPSET_INFO 按**索引**查，index 0 就是 capset 1，而 capset 1
本来就是小的那一个（几千字节的是 capset 2）。`tools/build-virglrenderer.sh` 里用
`STALE_CAPSET_BYTES=1024` 判断新旧量的是 capset 1，这个判据不成立。

宿主侧要拿到 context，需要三件事同时成立，缺一件都是**静默降级成 2D-only**：

1. EGL vendor 必须是 Mesa。双 GPU 宿主上 glvnd 默认选私有驱动，而它的 EGL 没有
   virgl 能用的 device/surfaceless 平台。
2. 必须把私有 GPU 的 `/dev/dri` 节点藏起来。Mesa 会枚举**所有** DRM 节点，私有 GPU
   的节点在 Mesa 下 `eglInitialize` 失败（`gbm device using incorrect/incompatible
   backend`），而 QEMU **不会**跳过它继续试后面的节点。
3. `egl-headless` 必须显式带 `gl=on`，否则 QEMU 直接拒绝创建设备。

这三条由 `tools/with-virgl-display.sh` 封好（用非特权 user+mount namespace，
不需要 root）。在这台宿主上用它跑 `tools/a20 test smoke-gpu3d-riscv64`，可以稳定
走到 `GET_CAPS returned a 308 byte capset`。

**XFCE GUI 实例（xfce-x86_64）实测**：会话能起来，`labwc` 认到
`Found config * for output Virtual-1`，并通过我们的 DRM/KMS 路径反复呈现
（`[DRM] present handle=N 1024x768`）。过程中修掉三处：

- `QEMU_GUI_DEVICES_x86_64` 没有键盘鼠标。wlroots 的 multi backend 先试 libinput，
  找不到输入设备就**放弃整个 session**（不会退回 DRM），而
  `start-xfce4-session` 不设 `WLR_LIBINPUT_NO_DEVICES`。
- `tools/a20 test` 对任何没写 `[machine]. memory` 的实例都 `NameError`
  （`d52c8ccb` 把三个常量挪进 `a20_resource.py` 却没加进 import）。
- PRIME 导出返回 fd 号所在namespace搞错，`-EIO`，wlroots 的
  `render/allocator/drm_dumb.c` 因此 "Failed to allocate buffer"。这是
  `e8149cad` 引入的回归，由 guest 日志逮到。

**还没有做到的，以及已知的下一步**（按当前证据排序，不猜）：

1. ~~`gpu3d_test` 的像素回读 4096/4096 全错~~ **已解决，见下面一节。**
2. guest 里跑 `eglinfo` 会 segfault（`ra=0x400`，解引用未映射的 `0x87613f40`），
   崩在 Mesa 的 EGL 设备枚举里。`libvirglrenderer.so.1` 与
   `virtio_gpu_dri.so` 都在镜像里，所以不是缺件；是 Mesa 侧与本驱动 UAPI 的交互
   还需要继续查。
3. 因此**还不能说"Mesa 已经挂上 virtio_gpu_dri"**。已有的是强旁证：强制
   `A20_RENDERER=gl`（wlroots 在 renderer 创建失败时是直接放弃而非退回）后 session
   仍然起来并呈现，且此前失败日志里的 `render/allocator/drm_dumb.c` +
   `render/swapchain.c` 正是 wlroots GL renderer 的代码路径。但这是推断，不是直接
   观测——`MESA_DEBUG=1` 没有输出，`egl-headless` 又不支持 `screendump`
   （它按设计就没有 display surface），所以还没有一张桌面截图作为直接证据。

A20OS 不自研着色器编译器，也不自研 DRI 驱动。GLSL→SPIR-V 由 Mesa 完成，
SPIR-V→host GPU 由 virglrenderer 完成；A20OS 要做的是把中间的运输层补齐
（GEM 对象模型 + 上游 virgl UAPI），让 stock Mesa 能挂上来。这是后续所有工作的出发点。

---

## 0.5 本轮（`feat/graphics-hardening`，续）：像素回读通了，根因不是命令长度

上面那条"`gpu3d_test` 像素回读全错、首像素是哨兵"现在**通过了**：
`tools/a20 test smoke-gpu3d-riscv64` 报 `pixel readback ok (0xffff0000 across
4096 pixels)` 与 `pixel readback ok (0xff0000ff across 4096 pixels)`，两遍颜色都中。

**此前"流里有一条命令长度不对"的结论是错的，且被测量排除。** 逐条核对
virglrenderer 1.3.0 的 `virgl_protocol.h` 与 `vrend_decode.c`，三条命令的长度
全都本来就对：`VIRGL_OBJ_SURFACE_SIZE` 5、`SET_FRAMEBUFFER_STATE` 要求
`2 + nr_cbufs` 即 3、`VIRGL_OBJ_CLEAR_SIZE` 8。日志里那个被当成"流错位证据"的
`329729`（0x00050801）其实是**中断路径回头上报的下一个 dword**，不是解码读错的
字段——它只是"这条命令被拒了"的后果，不是原因。

真正的原因是三个，其中第一个是主因：

**一、SUBMIT_3D 的命令头多带了一个 mem_entry，整条流因此被截断。** 这是主因。
宿主取命令体的代码是
`iov_to_buf(cmd->elem.out_sg, n, sizeof(struct virtio_gpu_cmd_submit), buf, cs.size)`
（QEMU `hw/display/virtio-gpu-virgl.c`，`virgl_cmd_submit_3d`）——它**跳过固定的
32 字节**再读命令体。而我们的 `submit_hdr` 曾经是
`{ struct virtio_gpu_cmd_submit hdr; struct virtio_gpu_mem_entry entry; } ALIGNED(64)`，
描述符长度 64。于是宿主跳 32 字节后，先把 `entry` 与对齐填充当成命令体的开头，
再只拿到 76 字节里剩下的 44 字节。**送进渲染器的一直是一段垃圾。** 而
SUBMIT_3D 依然回 OK——传输层确实成功了，渲染器静默丢弃而已，所以整条路径上
没有任何一处会报错。现在 `submit_hdr` 就是裸的 `virtio_gpu_cmd_submit`（32 字节），
并加了两条 `_Static_assert` 把这个线格式约束钉在编译期。

**二、从来没告诉过宿主"这个 resource 属于这个 context"。**
`VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE`（0x0202）在 UAPI 头里定义着，但驱动一次
也没发过。virglrenderer 的资源查找走的是 **context 自己的** 表
（`ctx->res_hash` / `vrend_renderer_ctx_res_lookup`），不是全局表，所以
`CREATE_OBJECT(SURFACE)` 引用的 resource 在 context 看来是"非法资源"。这一条被
探针直接证明：不调 `virgl_renderer_ctx_attach_resource` 就会得到
`Illegal resource 2` + `submit_cmd -> 22`，调了就是 `-> 0`。现在 EXECBUFFER 会为每个
`bo_handle` 补发这条命令。注意 SUBMIT_3D 的响应里**不携带** vrend 的解码状态，
所以这类失败天生对 guest 不可见。

**三、`TRANSFER_FROM_HOST` 之前是一个"校验后返回成功"的空实现。** 它的注释写
"同步完成的 submit 让 backing 已经是 coherent 的"，这句话是错的：宿主渲染进的是
它自己那份拷贝，**guest 的页从头到尾没人写**。所以这不是一致性bug，是那批字节
根本没被写过。现在这条 ioctl 真的下发 `VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D`，
并在返回前对覆盖到的页做一次 cache invalidate。

顺带修掉两处此前一直错的常量/期望值：
- `VIRGL_FORMAT_B8G8R8A8_UNORM` 是 **1**（2 是 `B8G8R8X8_UNORM`）。写错的是新写的
  探针，`gpu3d_test` 本来是对的。
- `gpu3d_test` 的两个期望像素值**是互换的**。B8G8R8A8 按地址升序存 B,G,R,A，
  所以红色的小端 u32 是 `0xffff0000`、蓝色是 `0xff0000ff`；原来写反了，即使渲染
  正确也会报 4096/4096 全错。
- 探针里 `PIPE_TEXTURE_2D` 应为 **2**（`enum pipe_texture_target` 第一个成员是
  `PIPE_BUFFER`），这一条上一轮已更正。

**门禁，而且实测会红。** 判据是 `smoke-gpu3d-riscv64` 要求出现
`pixel readback ok` 与 `PASS`。把那个 mem_entry 放回去（并临时关掉
`_Static_assert` 以便让**运行期**门禁去抓），门禁立刻转红，且复现的正是历史上的
那个签名：`FAIL pass 0: 4096/4096 pixels wrong, first pixel is 0xdeadbeef`。
`_Static_assert` 则把同一个错误挡在编译期。

**探针本身也修好了。** 它此前把 format 传成 2、没有 `make_current`、也没有
`ctx_attach_resource`，因此它"复现"的失败有一部分是它自己造出来的。现在它注册了
`virgl_set_log_callback`（QEMU 从不注册，所以 vrend 在 QEMU 里一声不吭），并按
QEMU 的真实顺序调用。附带结论：`make_current` 与 `submit_cmd(NULL,0,0)` 都不是
必需的，`ctx_attach_resource` 才是。

**仍未解决**：Mesa 侧的 `eglinfo` 段错误，以及"Mesa 到底挂上没有"缺一张直接证据
（见上面第 2、3 条）。3D 通路本身现在是有像素回读门禁的。

---

## 0.6 本轮：合并前阻塞项已解除，设备重选已修

**上一轮留下的唯一合并前阻塞项，本轮实测解除。** §0 那条"render node 越权"
是本分支里唯一一处会影响既有桌面的行为改动，文档要求上线前跑一次 `GPU_3D=0`
的 xfce smoke。这条要求此前一直没跑，所以它一直是"待验证"而不是"已验证"。

实测（`tools/a20 run xfce-x86_64`，该实例不设 `gpu_3d`，因此正是 2D-only 配置）：

- guest 日志 `[GPU] virtio-gpu 2D only (no VIRGL feature)`——确认走的是 2D 路径；
- `[DRM] present handle=1/2 1024x768` 反复出现，`pages=768 flush=0`；
- labwc 认到输出：`[main.c:282] Found config * for output Virtual-1`；
- XFCE 会话组件起来了（`xfsettingsd`、`xfdesktop`、缩略图服务经 D-Bus 激活）；
- **全程没有一行 master / render-node 错误**。

结论：把 KMS ioctl 收进 master 之后，2D 桌面照样工作。Linux 同样在 render node
上拒绝 `CREATE_DUMB`，而 GBM 本来就用 primary node 建 dumb buffer，所以没有
依赖旧行为的东西——这一点从实测得到确认，不再只是推理。

（附带一条：这个实例的镜像上一次运行留下了 ext4 校验和错误，preflight 拒绝启动
并要求先 rebuild。这是 `a20` 门禁在正常工作，不是缺陷。）

**另一处静默失效已修。** `gpu_device_unregister()` 原先只是把 `g_default_gpu`
清空就结束。若退位的设备走了而另一块显示设备还在线上，DRM 就此没有后端，
此后每个 ioctl 都回 `-ENODEV`，直到某次重探触发——而这个现象和"驱动坏了"
无法区分。现在它会遍历在线显示设备、用与 `gpu_device_register()` 相同的
晋级规则把槽位填回。两个容易写错的点：

- 扫描**不能**停在第一个候选。索引靠前的 2D-only 设备否则会赢过索引靠后的
  3D 设备，于是重选会把一个可用的 3D 绑定静默降级。只有拿到 3D 候选才停。
- 它**不抢占**扫描期间被并发填上的槽位：此时落地的 `register()` 已经做过选择。

选择规则本身抽到 `kernel/include/drivers/gpu/gpu_select.h`（两个布尔谓词），
`gpu_core.c` 调用它，因此被测的就是在跑的逻辑而不是一份拷贝。
`tools/tests/gpu_select_test.c` 由既有的 `tools/tests/*.c` 通配纳入
`make host-tests`，无需新增构建接线。门禁**实测会红**：把扫描改回"永不改进"、
拒绝填空槽、或让后探测者无论能力都获胜，三种改法分别退出 134。


---

## 1. 背景与原理

### 1.1 为什么需要 3D

- 现代桌面（GTK/Wayland 合成器、浏览器、媒体播放）依赖 GPU 合成与渲染。纯软件（pixman/cairo）能点亮屏幕，但把合成和几何变换压回 CPU，性能与功耗都不足。
- A20OS 运行在 QEMU 上，没有真实 GPU；可用的"硬件加速"是 virtio-gpu 的 3D/Context 模式：guest 通过 `VIRTIO_GPU_CMD_SUBMIT_3D` 提交 OpenGL 命令流，QEMU 侧 virglrenderer 在宿主机 GPU/CPU 上执行并回传结果。渲染计算发生在宿主机，guest 内核只负责搬运命令与资源。

### 1.2 virtio-gpu 两种模式

| 模式 | 命令 | 作用 | A20 状态 |
|---|---|---|---|
| 2D | `RESOURCE_CREATE_2D` / `TRANSFER_TO_HOST_2D` / `FLUSH` | 把像素块 blit 到 scanout | ✅ 现有 fbdev 路径 |
| 3D (virgl) | `CTX_CREATE` / `RESOURCE_CREATE_3D` / `SUBMIT_3D` / `TRANSFER_TO_HOST_3D` / BLOB | host 端 GL 上下文 + 命令流 | ⚠️ 命令封装已实现，但无 backing、无用户态客户端（见 §0） |

设备 feature 位：`VIRTIO_GPU_F_VIRGL (bit 0)` 表示 host 支持 3D；`VIRTIO_GPU_F_CONTEXT_INIT (bit 4)` 表示 context 初始化协议。

### 1.3 virgl 协议模型

- Context：一个 host 端 GL 状态机（类似 EGL 上下文），用 `ctx_id` 标识。
- Resource：host 端 GL 对象（纹理、帧缓冲、缓冲对象），用 `resource_id` 标识，带 `target/format/bind` 等 GL 参数。
- Submit：把一段 virgl 命令流（用户态 Mesa virgl 驱动打包的 GL 调用）经 controlq 交给 host。
- Capset：host 能力描述块（如 `VIRTIO_GPU_CAPSET_VIRGL`），描述协议版本与能力。

用户态 Mesa 的 `virtio_gpu` 驱动把 EGL/GLES 调用序列化进 command buffer，经 DRM ioctl 交给内核，内核原样转发给 virtio-gpu。命令流格式由 Mesa `virglrenderer` 的协议定义（`VIRGL_*` 编码）。

---

## 2. 内核接口

### 2.1 UAPI 头：`kernel/include/drivers/gpu/virtio_gpu.h`

该头定义全部 virtio-gpu 命令号、响应码、capset id 与数据结构，参照 Linux UAPI `virtio_gpu.h`：

- feature 位：`VIRTIO_GPU_F_VIRGL`、`VIRTIO_GPU_F_EDID`、`VIRTIO_GPU_F_CONTEXT_INIT`、`VIRTIO_GPU_F_RESOURCE_UUID`。
- 2D 命令：`GET_DISPLAY_INFO`、`RESOURCE_CREATE_2D`、`SET_SCANOUT`、`TRANSFER_TO_HOST_2D`、`RESOURCE_FLUSH`、`ATTACH/DETACH_BACKING`。
- 3D 命令：`GET_CAPSET_INFO`(0x0108)、`GET_CAPSET`(0x0109)、`RESOURCE_CREATE_BLOB`(0x010c)、`CTX_CREATE`(0x0200)、`CTX_DESTROY`(0x0201)、`CTX_ATTACH/DETACH_RESOURCE`(0x0202/3)、`RESOURCE_CREATE_3D`(0x0204)、`TRANSFER_TO/FROM_HOST_3D`(0x0205/6)、`SUBMIT_3D`(0x0207)、`RESOURCE_MAP/UNMAP_BLOB`(0x0208/9)。
- 响应码：`RESP_OK_NODATA/DISPLAY_INFO/CAPSET_INFO/CAPSET/EDID/RESOURCE_UUID/MAP_INFO` 与 `RESP_ERR_*`。
- capset：`VIRTIO_GPU_CAPSET_VIRGL`(1)、`VIRTIO_GPU_CAPSET_VIRGL2`(2)、`VIRTIO_GPU_CAPSET_VENUS`(4)、`VIRTIO_GPU_CAPSET_DRM`(6)。

### 2.2 （已删除）A20 私有 3D 透传 ioctl

历史记录：本节曾描述一组 A20 私有的 3D 透传 ioctl（`A20_GPU_IOCTL_*`，0x4700 段），
经 `gpu_dev_ops_t.ioctl` 转发给 GPU 驱动，参数统一为 `struct virtio_gpu_3d_req`：

| 曾存在的 ioctl | 语义 | 现在的等价物 |
|---|---|---|
| `VIRGL_CHECK` | 查询是否协商出 virgl | `DRM_IOCTL_VIRTGPU_GETPARAM` 读 `VIRTGPU_PARAM_3D_FEATURES` |
| `CTX_CREATE` | 创建 host 端 virgl 上下文 | `RESOURCE_CREATE` 内部惰性建 ctx；或 `DRM_IOCTL_VIRTGPU_CONTEXT_INIT` |
| `CTX_DESTROY` | 销毁上下文 | 随 open file 关闭 |
| `RES_CREATE_3D` | 创建 3D 资源 | `DRM_IOCTL_VIRTGPU_RESOURCE_CREATE` |
| `RES_UNREF` | 释放资源 | GEM 回收时自动（`drm_gem_reclaim`） |
| `SUBMIT_3D` | 提交命令流 blob | `DRM_IOCTL_VIRTGPU_EXECBUFFER` |

删掉的理由很直接：这组 ioctl 与上游 VIRTGPU UAPI 逐条重复。维护两套 3D ABI 意味着两条
可能各自漂移的代码路径，而**只有上游那套是 Mesa 真正会说的**，花在私有这套上的每一小时
都不能被任何真实客户端用到。

删除时唯一被刻意保留下来的是 `gpu3d_test` 的三态退出码：`77`（SKIP，2D-only 设备）
**是门禁可证伪性的来源**，否则"没有 virgl 的设备"与"virgl 坏了的设备"无法区分，门禁会空转
通过。这个信息现在从上游 `VIRTGPU_PARAM_3D_FEATURES` 读；该参数原先被硬编码为 1，
必须先让它如实报告已协商的 feature 位，SKIP 才重新可达。

### 2.3 分发路径

```
用户态 (Mesa virtio_gpu_dri.so / gpu3d_test)
   │ ioctl(/dev/dri/card0, DRM_IOCTL_VIRTGPU_*)
   ▼
kernel/fs/devfs/devfs.c   DEVFS_DRM open → drm_create_vfile()
   ▼
kernel/drivers/gpu/drm.c drm_ioctl() → drm_virtgpu_*
   │  解析 GEM handle、attach backing、copy 命令流（内核不解码 virgl 字节）
   ▼
kernel/drivers/gpu/virtio_gpu.c 的 gpu_dev_ops_t 3D ops
   │  ctx_create / resource_create_3d / resource_attach_backing / submit_3d
   ▼
virtio_gpu_send_cmd() / virtio_gpu_send_cmd_big() / virtio_gpu_submit_3d()
   │  controlq 描述符链 + MMIO notify
   ▼
QEMU virtio-gpu-gl → virglrenderer → 宿主 GPU/CPU
```

---


## 3. 内核驱动实现

### 3.1 feature 协商（`virtio_gpu_init_transport`）

- 读取 device features 低 32 位，协商 `VIRTIO_GPU_F_VIRGL` 与 `VIRTIO_GPU_F_EDID`；高 32 位只协商 `VIRTIO_F_VERSION_1_BIT`。
- 协商结果记录在 `inst->virgl` 与 `inst->context_init`。
- 与 2D 一样 setup controlq（queue 0），发送请求-响应对。

### 3.2 命令传输

- `virtio_gpu_send_cmd()`：固定 128 字节命令槽，实例内 `command_req/command_resp`，用于 capset info、ctx、resource 等小命令。
- `virtio_gpu_send_cmd_big()`：动态增长 `big_req/big_resp`，用于 `GET_CAPSET` 大 blob。
- `virtio_gpu_submit_3d()`：三描述符链（hdr+entry → 命令 blob → 响应），命令 blob 拷入 `big_req` 保证 DMA 安全。禁止把调用者栈对象直接写入 descriptor。
- 所有命令在 `command_lock` 串行；等待期间若注册了 IRQ 则 park，否则有界轮询 + yield。

### 3.3 3D 命令封装

| 封装 | 对应命令 | 说明 |
|---|---|---|
| `virtio_gpu_get_capset_info` | `GET_CAPSET_INFO` | 查 capset id/version/size |
| `virtio_gpu_get_capset` | `GET_CAPSET` | 拉取 capset blob |
| `virtio_gpu_ctx_create` | `CTX_CREATE` | 建 host GL 上下文 |
| `virtio_gpu_ctx_destroy` | `CTX_DESTROY` | 销毁上下文 |
| `virtio_gpu_resource_create_3d` | `RESOURCE_CREATE_3D` | 建 GL 资源 |
| `virtio_gpu_resource_unref` | `RESOURCE_UNREF` | 释放资源 |
| `virtio_gpu_submit_3d` | `SUBMIT_3D` | 提交命令流 |

### 3.4 DRM 透传（`kernel/drivers/gpu/drm.c`）

`drm_ioctl()` 按 `DRM_IOCTL_VIRTGPU_*` 直接分发到 `drm_virtgpu_*`，后者再经 `gpu_dev_ops_t` 的 3D ops 触达驱动。`/dev/dri/card0` 是 3D 与 KMS 的统一入口；`default` 分支一律 `-EINVAL`（私有 3D 段已删除，见 §2.2）。

### 3.5 drvmod 导出

`kernel/drvmod/framework.c` 的 `drv_export_table` 新增 `copy_from_user`/`copy_to_user`，供 virtio-gpu 模块透传用户请求。

### 3.6 一个已知约束：PIC 跳转表

drvmod 模块以 `-fPIC` 编译，`gpu_ioctl` 若用 `switch` 分发会生成 PIC 跳转表（`R_RISCV_ADD32/SUB32`），drvmod loader 不支持该类 reloc，所以**必须用 if-chain 分发**（代码中已注释说明）。新增 3D 命令时不要在该函数里引入 `switch`。

---

## 4. 用户态开发指南

### 4.1 自建测试：`gpu3d_test`

`user/cmds/core/gpu3d_test.c` 是验证内核 3D 链路的独立工具，ioctl 号与结构在文件内自包含（不依赖内核头）。它需要 virgl-capable 设备（`GPU_3D=1`）：

```sh
# 推荐走门禁（见 gpu-3d-roadmap.md §8）
tools/a20 test smoke-gpu3d-riscv64
# 实测期望输出：
#   [GPU] virtio-gpu 3D (virgl): capset[0] id=1 ver=1 size=308 ctx_init=1
#   GPU3D_TEST: virgl available
#   GPU3D_TEST: context 1 created
#   GPU3D_TEST: 3D resource 2 created (16x16 RGBA8)
#   GPU3D_TEST: resource 2 released
#   GPU3D_TEST: context destroyed
#   GPU3D_TEST: PASS (transport only -- no command stream submitted, rendering unverified)
```

退出码是三态，且必须保持三态：

| 码 | 含义 |
|---|---|
| 0 | PASS：virgl 可用，且每一步传输都被 host 接受 |
| 77 | SKIP：设备 2D-only，没有 3D 通路可测（autotools 约定） |
| 1 | FAIL |

2D-only 设备输出 `GPU3D_TEST: SKIP 2D-only device` 并返回 77。旧版本在这里
`return 0`，于是该测试在任何配置下都是绿的，绿灯不携带任何信息，这正是
§0 那两条失真结论的来源。SKIP 故意不等于 PASS。

PASS 那行有自我限定：**它没有提交命令流，资源也没有 backing**，
所以它证明的是*传输可达性*，不是"渲染成功"。命令流与 backing 见
[gpu-3d-roadmap.md §7、§12](gpu-3d-roadmap.md)。

### 4.2 完整的 virgl 客户端栈（后续阶段）

内核透传只是搬运层。要跑起真实 GL 应用，用户态需要四层：

1. libdrm：`drmOpen` `/dev/dri/card0`、dumb-buffer 管理（由 Alpine `libdrm` 包提供）。
2. libgbm：GBM 提供 EGL 平台抽象；Mesa 的 `virtio_gpu` 后端把 GBM surface 映射到 virgl resource。
3. Mesa（EGL/GLES2）：`eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm_dev, ...)` 创建 EGL display；virgl 驱动把 GL 调用序列化进 command buffer，经 `DRM_IOCTL_VIRTGPU_EXECBUFFER` 提交。
4. 合成器/应用：Wayland 合成器（Weston 的 DRM backend + EGL 渲染器）或直接 EGL 客户端。

对接时内核侧需要补充的部分（按依赖顺序）：

- `RESOURCE_ATTACH_BACKING` / `TRANSFER_TO_HOST_3D` 透传（资源数据上传）。
- `BLOB` 命令（`RESOURCE_CREATE_BLOB` / `MAP_BLOB`）用于共享内存资源。
- DRM 侧 `GETFB2` / `PRIME_HANDLE_TO_FD` / `SYNCOBJ` 等与 GBM 深度配合的 ioctl。

### 4.3 运行环境

- 3D 需要 QEMU 以 `virtio-gpu-gl-*` 设备启动，并带 OpenGL 显示后端（如 `-display egl-headless`）。
- 检查宿主 QEMU 是否编译了 virgl：`qemu-system-riscv64 -device help | grep virtio-gpu-gl`，并确认 `libvirglrenderer.so` 存在。
- 2D 模式（`virtio-gpu-device`）不受影响，内核自动回退。

---

## 5. 验证与验收

内核侧验收（已完成）：

```sh
# riscv64 dev 镜像 + virtio-gpu-gl-device + -display egl-headless
(sleep 8; printf 'gpu3d_test\npoweroff\n') | qemu-system-riscv64 \
    -machine virt -m 1G -nographic -smp 1 -bios default \
    -global virtio-mmio.force-legacy=false \
    -drive file=.../fat32.img,if=none,format=raw,id=x0 \
    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
    -display egl-headless -device virtio-gpu-gl-device,bus=virtio-mmio-bus.7 \
    -kernel .../kernel.elf
```

boot 日志应出现：

```
[GPU] virtio-gpu 3D (virgl): capset[0] id=1 ver=1 size=308 ctx_init=1
[GPU] virtio-gpu ready: 1024x768 (FB: 3 MB at 0x...)
```

随后 `gpu3d_test` 输出 PASS。2D 回归：用 `virtio-gpu-device` 应输出 `2D only (no VIRGL feature)` 且 `gpu3d_test` 跳过。

---

## 6. 文件索引

| 文件 | 内容 |
|---|---|
| `kernel/include/drivers/gpu/virtio_gpu.h` | virtio-gpu UAPI：命令/响应/capset/结构 + A20 3D ioctl 与 `virtio_gpu_3d_req` |
| `kernel/drivers/gpu/virtio_gpu.c` | 驱动：feature 协商、capset、3D 命令封装、`gpu_ioctl` 分发 |
| `kernel/drivers/gpu/drm.c` | DRM `/dev/dri/card0`：KMS + A20 3D 透传 |
| `kernel/drvmod/framework.c` | drvmod 导出表（含 `copy_from_user/to_user`） |
| `user/cmds/core/gpu3d_test.c` | 用户态 3D 自测（走门禁 `tools/a20 test smoke-gpu3d-riscv64`） |
| `user/cmds/core/egl_test.c` | 已删除（曾被 `user/Makefile` 从 `LOCAL_CMD_SRCS` filter 掉） |
| `tools/check-drm-abi.sh` | DRM UAPI 门禁：ioctl 号 + 结构体布局对 Linux UAPI 双向可证伪（§8.1） |
| `tools/build-virglrenderer.sh` | 宿主侧 virglrenderer 构建（已运行，装出 1.3.0；Mesa 仍因宿主 EGL 未 attach） |
| `tools/a20_resource.py` | 启动前宿主资源门禁（RAM/负载/磁盘；a20_preflight.py 已合并进去） |
| `docs/graphics/gpu-3d-roadmap.md` | 路线图与排序论证 |
| `docs/graphics/real-hardware-gpu.md` | 真机 GPU 的现实边界（scanout vs 3D 加速） |
| `docs/graphics/host-tools.md` | 上面三个宿主工具的用法 |
| `user/Makefile` | `gpu3d_test` 编译规则 |
| `docs/drivers/classes/display.md` | Display 类总文档（2D + 3D 综述） |

---

## 7. 相关设计约束

- DMA 安全：所有交给 virtio-gpu 的描述符与数据必须来自驱动持有的稳定内存（实例成员或 `kmalloc`），不能引用用户栈/调用者栈。
- 同步命令串行：controlq 一次一个 in-flight 链，用 `command_lock` 保护；等待期间保持中断开启，避免 `spin_lock_irqsave` 包裹设备完成等待。
- reloc 限制：drvmod 模块的 `gpu_ioctl` 必须用 if-chain，不用 switch（PIC 跳转表 reloc 不被 loader 支持）。
- 2D/3D 共存：2D fbdev 路径与 3D 透传路径互不干扰；无 virgl 时自动回退 2D。

---

## 8. 用户态 GL（Mesa/EGL/GBM）的前置条件

内核侧 3D 命令透传（第 2 节）可用，`xfce` world 现在也装了 Mesa
（`mesa`/`mesa-dri-gallium`/`mesa-gl`/`mesa-gles`/`mesa-egl`/`mesa-gbm`/
`virglrenderer` + `mesa-utils`/`mesa-demos`）。镜像里能查到
`swrast_dri.so`/`kms_swrast_dri.so`/`virtio_gpu_dri.so`/`zink_dri.so` 与
`libGL.so.1`/`libEGL.so.1`/`libgbm.so.1`/`libGLESv2.so.2`。但**用户态 GL 仍然起不来**，
缺的是 DRM 侧的三样东西：

| 缺口 | 为什么需要 | 现状 |
|---|---|---|
| 可渲染缓冲的分配 | Mesa/GBM 用它分配「可渲染」缓冲并 mmap 到用户态；没有它 `gbm_bo_create(GBM_BO_USE_RENDERING)` 直接失败 | **Linux UAPI 里没有 `GEM_CREATE`/`GEM_MMAP`**：用户态分配缓冲走 `MODE_CREATE_DUMB`，本分支已实现。命名 handle 用 `GEM_FLINK`（真 UAPI ioctl，8 字节），导入用 `GEM_OPEN`（真 UAPI ioctl）。见 §8.1 |
| render node `/dev/dri/renderD128` | 没有 DRM master 的普通程序要打开 GPU 只能靠 render node；`EGL_PLATFORM=surfaceless` 也要它 | 已实现：devfs 暴露 `renderD128`（226,128），打开它的 DRM 上下文标记为 render-only（`SET_MASTER` 返回 `-EACCES`，KMS ioctl 本就需要 master）。实测加上后 EGL 能在 Wayland 平台初始化（`EGL driver name: swrast`，且带 `EGL_EXT_image_dma_buf_import`） |
| `MODE_GETFB2` | Mesa/合成器导入 framebuffer（XWayland/DRI3 等） | 已实现并对齐 UAPI（ioctl 号 `0xc06864ce`，此前是错的 `0xc04864ca`，`drmModeGetFB2` 恒 `EINVAL`；见 §8.1）。内核从后备 GEM 应答，不是一个清零 stub |
| plane IN_FORMATS blob | Mesa 的 DRM 平台用它构造 EGL config 列表；wlroots 也用它取 plane 的格式集 | 已实现（`adeffcb9`）：plane 现在带 `type`(id=2) + `IN_FORMATS`(id=3) 两个属性，IN_FORMATS 指向一个 56B `drm_format_modifier_blob`（header 24B / formats@24 / modifier@32，ARGB8888+XRGB8888、LINEAR）。boot 验证：桌面正常起来（page-flip 正常、xfsettingsd/xfdesktop 都在、不再 `Failed to create DRM backend`）。注意 wlroots 只在用 GL 渲染器时才懒读这个 blob：`WLR_RENDERER=pixman` 下根本不取它，所以本改动对当前桌面零影响。blob 是否被 wlroots/Mesa 正确解析，要等 GL 渲染器链路打通后才能端到端验证。 |

本仓库没有 vendored 的 Mesa/libdrm/EGL/GBM/wlroots/labwc/Xwayland/XFCE 源码。
上面说的"用户态"是 stock Alpine apk world（`packages/world/xfce.world`），
跑在 Linux syscall ABI（`kernel/abi/linux/syscall_table.def`）之上，经 Linux
ioctl syscall 访问 `/dev/dri`。两个常见的"用户态 DRM 定义"引用都不成立：

- `user/external/mlibc/sysdeps/managarm/generic/drm.cpp` **不在构建路径里**。
  A20OS 用 `tools/targets-mlibc.mk` 配置 mlibc 的 `sysdeps/a20`，其中没有任何
  DRM 代码。
- `user/cmds/core/egl_test.c`（已删除）曾被 `user/Makefile` 从 `LOCAL_CMD_SRCS` 里 filter
  掉，**不参与构建，是死代码**。不要把它读成"A20OS 有 EGL 自测能力"。

唯一真正的用户态 DRM 定义来源是 Alpine 那份 libdrm 及其安装的
`<drm/*.h>` UAPI 头，这正是 §8.1 的门禁所比对的基准。

| plane IN_FORMATS blob | Mesa 的 DRM 平台用它构造 EGL config 列表；wlroots 也用它取 plane 的格式集 | 已实现（`adeffcb9`）：plane 现在带 `type`(id=2) + `IN_FORMATS`(id=3) 两个属性，IN_FORMATS 指向一个 56B `drm_format_modifier_blob`（header 24B / formats@24 / modifier@32，ARGB8888+XRGB8888、LINEAR）。boot 验证：桌面正常起来（page-flip 正常、xfsettingsd/xfdesktop 都在、不再 `Failed to create DRM backend`）。注意 wlroots 只在用 GL 渲染器时才懒读这个 blob：`WLR_RENDERER=pixman` 下根本不取它，所以本改动对当前桌面零影响。blob 是否被 wlroots/Mesa 正确解析，要等 GL 渲染器链路打通后才能端到端验证。 |

IN_FORMATS 四次尝试的二分结论，关键在触发器不是 blob，而是「plane 报了多于一个属性」：

| 变体 | 结果 |
|---|---|
| 裸 blob（IN_FORMATS，BLOB+LINEAR） | `Failed to create DRM backend` |
| blob + `MODE_GETPLANE` 报同样两个 fourcc | 同样失败 |
| blob + `WLR_DRM_NO_MODIFIERS=1` | 同样失败 |
| 属性改名为 `FOO`（wlroots 认不出） | 同样失败 → 不是名字 |
| 属性改成 ENUM（不是 BLOB） | 同样失败 → 不是 BLOB 类型 |
| 两个属性都是已知可用的 `type`/PRIMARY | 同样失败 → 不是第二个属性的处理代码 |

并且：在全部失败变体里，**IN_FORMATS 的 blob 从未被取过**（我在 `drm_in_formats_blob()` 里加的打印一次都没出现），说明失败发生在 wlroots 读 plane 属性列表的阶段，早于格式/blob 解析；wlroots 也没有打出任何具体错误行（静默失败）。所以问题不在内核返回的 blob 字节，而在「这个驱动上 plane 有 2 个属性」这一事实本身触发 wlroots/libdrm 的某个前置检查失败。

> 更正（adeffcb9，已落地）：上面这条「plane 有 2 个属性就失败」的结论是错的。后来用一个干净的「两个 `type` 属性」复现（count_props=2）发现 wlroots 完全正常（桌面起来、page-flip 正常）；多属性列表编码从来不是问题，之前那几次 IN_FORMATS 失败是那些尝试自身的接线/编码 bug（与本会话里我几次探针自身的栈 bug 同理）。`adeffcb9` 的实现让 plane 同时暴露 `type` + `IN_FORMATS`，boot 验证桌面无恙。IN_FORMATS 这条路本身已经通了；剩下要验证的是 blob 内容被 wlroots/Mesa 正确解析（需 GL 渲染器链路），以及后续的 GBM/EGL/真 PRIME。

下一步（下次接手时的第一件事）：写一个最小用户态程序，用 libdrm 调 `drmModeGetPlane()` + `drmModeObjectGetProperties()`，把 1 属性与 2 属性两种情况的返回值打出来；或直接读 wlroots 的 plane 属性循环源码（本机网络取不到，需离线准备）。

上一轮用临时 ioctl 打印得到的调用序列：

- wlroots 调用序列（失败前最后一段）：`GETRESOURCES` → `GETCRTC` → `OBJ_GETPROPERTIES`×2 → `GETPLANERESOURCES`×2 → `GETPLANE`×2 → `OBJ_GETPROPERTIES`×2 → `GETPROPERTY`×4（= plane 两个属性各两次「先取长度再取值」）→ `OBJ_GETPROPERTIES`×2 → `DROP_MASTER`(0x641f) → 失败。
- 最后的 `DROP_MASTER` 是 libdrm `drmGetNodeTypeFromFd()` 的 node-type 探测，即它发生在一个新打开的 fd 上，对应 wlroots 的 allocator 阶段（`wlr_drm_allocator_create`：先试 `drmModeCreateLease`，失败后 plain open 再探测），而不是 plane 循环。所以失败点在 allocator（dumb/gbm 后端用 plane 的格式集），不在属性本身。
- 全流程里 wlroots 唯一未实现的 ioctl 是 `DRM_IOCTL_MODE_CREATE_LEASE`（`0xc6`），它自己会 `falling back to plain open`，非致命。
- 下一步建议：写个用户态小程序 `drmModeGetPropertyBlob(IN_FORMATS)` + `drmModeGetPlane`，把内核返回的字节与 wlroots/Mesa 的解析逐字段对齐；或先只让 dumb allocator 的格式来源自洽（GETPLANE 与 IN_FORMATS 完全一致、并确认 modifier 解析出 XRGB8888+LINEAR），再往上试 GBM。
| 真正的 dma-buf（PRIME） | 客户端把渲染结果当 `wl_buffer` 交给合成器（`zwp_linux_dmabuf_v1`）；dumb buffer 导不出 dma-buf | `DRM_IOCTL_PRIME_HANDLE_TO_FD` 现在是把缓冲内容 memcpy 进 memfd 再返回该 fd（`drm_prime_handle_to_fd`），不是可共享的 dma-buf |
| `DRM_IOCTL_MODE_GETFB2` | Mesa/合成器导入 framebuffer（XWayland/DRI3 等） | 已实现（ioctl 号与内核结构体均已对齐 UAPI，见 §8.1）。此前 ioctl 号错误，`drmModeGetFB2` 恒返回 `EINVAL`；再此前它是一个清零 stub |

实测（guest 内 `eglinfo`）：

```
libEGL warning: failed to get driver name for fd -1
libEGL warning: MESA-LOADER: failed to retrieve device information
MESA: error: ZINK: vkCreateInstance failed (VK_ERROR_INCOMPATIBLE_DRIVER)
libEGL warning: egl: failed to create dri2 screen
```

直接后果：

- 合成器只能跑 pixman：`start-xfce4-session` 写死 `WLR_RENDERER=pixman`，
  因为 wlroots 的 GL 渲染器要先 `gbm_create_device()` +
  `eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR)`，上面几样缺一不可。
  XWayland 也只能 `Failed to initialize glamor, falling back to sw`。
- GL 客户端（Minecraft 等）无法出图：即使 llvmpipe 能软件渲染，结果也
  无法作为 dma-buf 交给合成器。
- host 侧也没有 3D：GUI 实例用 `-device virtio-gpu-pci`（无 virgl），
  所以 `VIRTGPU_PARAM_3D_FEATURES` 报 0、3D ops 返回 `-ENXIO`；要试硬件 3D 得换
  `virtio-gpu-gl-pci`（或 `virgl=on`）。

### 让 Minecraft 跑起来的顺序

> 本小节已被 §9.11 与 §9.22 推翻并取代，保留原文作为调查过程记录。
> 下面这份"顺序"假定卡点在 `GEM_CREATE`/`GEM_MMAP`/render node/`GETFB2`
> 那条 DRM 链上。这四项现已全部落地（见 §8.1），而 Minecraft 依然没有
> 停在它们上。真正的卡点是呈现：XWayland 的窗口不被呈现。现行做法是绕过
> 它（让 GLFW 选 Wayland 后端），而不是修它。

1. 内核补 `GEM_CREATE`/`GEM_MMAP`（把现有 vmo 暴露成 GEM 对象）+ render node
   + `MODE_GETFB2`；  ← 已完成，但清单本身是错的：`GEM_CREATE`/`GEM_MMAP`
   不是 Linux UAPI 概念，Linux 用 `MODE_CREATE_DUMB` 分配缓冲（见 §8.1）
2. 把 PRIME 做成真 dma-buf（或至少让 `kms_swrast` 的 dumb buffer 能被合成器
   直接采样）；  ← 未完成，且真 dma-buf 在 A20OS 上是从零做（无跨进程 VMO
   共享、无 mmap-offset 协议）
3. 放开 `WLR_RENDERER`，让 wlroots 用 GL 渲染器（llvmpipe 软件渲染先跑通）；  ← 仍待做
4. 再考虑 virgl：host 开 `virtio-gpu-gl-pci`，guest 用 `virtio_gpu_dri.so`；  ← 被宿主 renderer 版本挡住
5. 注意 LWJGL 只提供 x86_64/aarch64 native，riscv64 实例跑不了 Minecraft
     （Java 本身可以）。

现行顺序见 [gpu-3d-roadmap.md §9](gpu-3d-roadmap.md) 与
[gpu-3d-roadmap.md §9.1](gpu-3d-roadmap.md)（Wayland 绕过）。

### GBM/EGL 的实测定位（本轮）与一条可能更短的路

- `eglinfo -p gbm` 现在能给出精确失败点：`eglInitialize` → `DRI2: failed to create gbm device`。
  即 Mesa 的 GBM 后端（镜像里有 `/usr/lib/gbm/dri_gbm.so`）在为本驱动的设备建 `gbm_device` 时失败。
  `MESA_LOADER_DRIVER_OVERRIDE=kms_swrast` 和 `swrast` 都试过，都不改变结果，说明卡点不在
  「驱动名→DRI 驱动」的映射，而在 dri_gbm 的设备初始化本身（它对本 DRM 设备的某些查询/调用不满意）。
  下一步要么在 guest 里 strace（已加入 `xfce.world`，无需再离线准备）看 gbm_create_device 里哪一步失败，要么在内核 DRM ioctl
  分发上挂临时 trace 看 Mesa 在初始化阶段问了什么、哪条答得不对。
- 可能更短的路（待验证，不要当结论）：Minecraft 走 LWJGL → `EGL_PLATFORM_WAYLAND`，而 EGL 在 Wayland
  平台已经能初始化（swrast）。真正的问号是呈现：eglSwapBuffers 时 Mesa 把渲染结果作为 `wl_buffer` 交给
  合成器。若 Mesa 的 Wayland EGL 对软件渲染用 `wl_shm`（CPU 共享内存，wlroots 原生支持）而非 `wl_dmabuf`，
  那 Minecraft 也许根本不需要 GBM/真 dma-buf 这条链。验证方法：跑一个真正的 EGL-Wayland 客户端
  （镜像里没有编译器，也无 weston-simple-egl，需要离线准备一个静态 musl 的 Wayland EGL 测试程序）。
- 另一个独立于 GL 的硬前提：LWJGL natives + Minecraft jars 不在镜像里，离线取不到，需另行准备。
- 补测（环境侧已齐备，"缺驱动"这条排除）：镜像 `/usr/lib/dri/` 里其实全都有，`swrast_dri.so`、
  `kms_swrast_dri.so`、`virtio_gpu_dri.so`、`zink_dri.so`，以及 crocus/i915/iris/nouveau/r300/r600/radeonsi/vmwgfx；
  GBM 后端 `/usr/lib/gbm/dri_gbm.so` 也在。即便如此，带 `MESA_LOADER_DRIVER_OVERRIDE=kms_swrast` 的
  `eglinfo -p gbm` 仍然 `DRI2: failed to create gbm device`，而且内核侧没有任何 DRM ioctl 返回错误
  （用临时 trace 覆盖了整个 ioctl 分发验证过）。卡点在 Mesa `gbm_create_device()` 内部（设备识别/后端
  初始化），既不是缺文件也不是内核答错。要继续需要 Mesa 源码，或在 guest 里 strace（已加入 `xfce.world`）
  看它到底哪一步返回 NULL。
- 真正的卡点定位（libdrm 读不到设备身份）：`MESA-LOADER: failed to retrieve device information` 出在 libdrm 的
  `drmGetDevice2()`。从 `libdrm.so.2` 的字符串可以读出它要访问的 sysfs 路径：
  `/sys/dev/char/<maj>:<min>` → `/device` → `/device/drm`，以及
  `/sys/bus/pci/devices/<bdf>/` + `<bdf>/config` + `<bdf>/uevent`。
  而客体里实测：`/sys/bus` 根本不存在；`/sys/dev/char/226:0` 的 readlink 是 `../../class/drm/card0`
  （路径里没有 `pci`，libdrm 因此识别不出这是 PCI 设备）；`/sys/dev/char/226:0/device` 是个空目录
  （没有 `uevent`/`config`）。libdrm 拿不到 vendor/device，Mesa loader 无法把设备映射到 DRI 驱动，
  `gbm_create_device()` 于是返回 NULL，往外就是 `DRI2: failed to create gbm device`。
- 修法（内核 sysfs，尚未动手）：把 DRM 设备的 sysfs 摆成 Linux 的形态。`/sys/dev/char/226:0` 应指向
  `/sys/devices/pci0000:00/<bdf>/drm/card0`，在 `<bdf>/` 下提供 `uevent`（含 `PCI_ID=1AF4:1050` 等）、
  `config`（PCI 配置空间）、`vendor`/`device`，并提供 `/sys/bus/pci/devices/<bdf>` 的链接。
  内核侧信息是齐的（`pci_dev_info` 带 vendor/device，`pci_slot_for_bdf()` 带槽位）。
  注意这会动到现有 `/sys/class/drm/*` 布局，而 wlroots/libinput 现在依赖它：改之前必须先跑桌面回归，
  别把已经能用的桌面弄坏。而且这只是 GL 链的第一环，后面还有 kms_swrast 建 screen、EGL、真 dma-buf PRIME、
  MODE_GETFB2、以及放开 `WLR_RENDERER`。

### 8.1 本轮查出的 UAPI ABI 缺陷：错的 ioctl 常量是隐形的

这一节是全文最可迁移的内容。此前文档把这些缺陷的症状当成 Mesa 或
libdrm 的问题来推理；它们全部是 A20OS 侧的常量与结构体错误。

`kernel/include/drivers/gpu/drm.h` 把 ioctl 号写成字面十六进制（它们落在
switch 里，不走宏）。于是一个写错的常量不会报错：驱动的 switch 匹配不上，
ioctl 落到 `default` 分支，用户态看到 `EINVAL`/`ENOTTY`。症状指向 Mesa，
病灶是一个头文件里的一个字符。本仓库的这些号肉眼核对过，而肉眼核对
一次都没有发现它们漂移。

| 项 | A20OS 原值 | Linux UAPI | 后果 |
|---|---|---|---|
| `MODE_GETFB2` | `0xc04864ca` | `0xc06864ce` | size 字段与命令号都错，`drmModeGetFB2` 恒返回 `EINVAL` |
| `MODE_RMFB` | `_IOW` | `_IOWR` | `drmModeRmFB` 永不匹配 |
| `GEM_OPEN` | `0xc0186410` | `0xc010640b` | 永不匹配 |
| `struct drm_gem_open` | handle 在 offset 8 | handle 在 4、size 在 8 | 每次 `GEM_OPEN` 都从错误字段读 handle |
| "GEM_GET_HANDLE" | 12 字节结构 | Linux 的 `GEM_FLINK` 是 8 字节 | 结构体大小与 UAPI 不符 |
| `GEM_MMAP` | `0xc010640b` | 这是 Linux 的 `GEM_OPEN` | 命名空间撞车：guest 调真 `GEM_OPEN` 被 mmap handler 应答 |

最后一条最严重，因为它**不是笔误而是命名空间撞车**。成因是：

> Linux UAPI 里根本没有 `GEM_CREATE`、`GEM_MMAP`、`GEM_GET_HANDLE`。
> 用户态分配缓冲走 `MODE_CREATE_DUMB`；给导入用的 handle 起名走 `GEM_FLINK`。
> 所以那三个 A20OS 号**没有上游对应物可对齐**，而 A20OS 当时正是"照 Linux 的
> 样子"猜的号，于是一个私有号落到了真 `GEM_OPEN` 的位置上。

现状：这三个里 `GEM_FLINK` 已作为真 ioctl 实现；仍保留的两个是 **A20 私有**，
落在 `0x4710` / `0x4711`，不与任何 Linux 号冲突。内核侧实测共 49 个 DRM
ioctl（7 device/auth、5 GEM、2 PRIME、25 KMS、10 VIRTGPU）加一个 16 槽的
A20 私有号段。

同时更正一条旧文档的错误结论：早期版本与旧路线图称 `MODE_GETFB2`
"已加"，因此"不是空洞"。它是空洞：ioctl 号错了，从来没被命中过。

门禁是 `tools/check-drm-abi.sh`（用法见
[host-tools.md](host-tools.md)），它做两件事，缺一不可：

1. 把 `drm.h` 里的 `DRM_IOCTL_*` 名字抓出来，针对本机
   `/usr/src/linux-headers-*/include/uapi/drm` 编译探针打印上游值再逐条比对。
   不问编译器就等于重新实现一遍 `_IOWR()`，而结构体大小正是这里的关键。
2. 从 `drm.c` 里把结构体定义 lift 出来编译并量 `sizeof` / `offsetof`。
   正确的 ioctl 号配错误的布局照样静默破坏数据：libdrm 按 UAPI 说的偏移写
   字段，驱动按自己那份结构体读，两边都"成功"。

实测：48 个号与 Linux UAPI 一致，1 个无上游对应（`MODE_DPMS`，上游同样没有
定义）。**两个方向都实测会失败**：扰动 `MODE_RMFB`、从 `drm_mode_fb_cmd`
删掉一个字段，都会被抓出来。漂移 exit 1，通过 exit 0，没装 Linux 头文件时
干净地 skip。

---

## 9. libdrm 设备识别所需的 sysfs 形态（反汇编实证，可直接照做）

上一节的结论来自字符串，本节把它升级为逐条反汇编核实的事实。样本：客体实际使用的
`libdrm 2.4.131`（从 `build/cache/apk/x86_64/libdrm-2.4.131-r0.400a5d6e.apk` 取出）。
复现命令：

```bash
cd /tmp/opencode && mkdir drmx && tar -xzf <apk> -C drmx
objdump -d --no-show-raw-insn drmx/usr/lib/libdrm.so.2.131.0 > drm.asm
python3 -c "d=open('libdrm.so.2.131.0','rb').read(); print(repr(d[0x10a08:0x10a30].split(b'\0')[0]))"
```

### 9.1 调用链（Mesa 的 `gbm_create_device` 走的就是这条）

`drmGetDevice2(fd)` → `drmGetDeviceFromDevId(dev_id, flags, &dev)`：

1. 枚举 `/dev/dri/*`（字符串 `/dev/dri`，0x101e6；名字前缀匹配 `card`(4) / `renderD`(7)，0x101fc/0x10201）。
2. 对每个 node 先 `stat("/sys/dev/char/<maj>:<min>/device/drm")`（格式串 0x10a08）。
   失败即 `-EINVAL` 直接返回。这是整条链的第一个、也是最硬的失败点。
3. `drmGetNodeType(maj, min)`：`snprintf("/sys/dev/char/<maj>:<min>/device")`（0x1021f）→ 解析该路径
   → 用一张 7 项表把路径后缀映射成 bus 类型（见 9.2）。它必须被识别成某一种 bus，否则 node 类型
   未知、直接失败。
4. `drmGetDeviceName()`（0x66a0）：`realpath("/sys/dev/char/<maj>:<min>/device")`；若最后一段以
   `/virtio` 开头则截断到它的父目录（对应 Linux 上 `.../0000:00:01.0/virtio0` 的经典布局）。
5. PCI 分支：从第 4 步得到的目录读 5 个属性文件（表 @0x14840，`fscanf("%x")` 0x1031e）：
   `revision`、`vendor`、`device`、`subsystem_vendor`、`subsystem_device`。任一读不到即失败。
6. PCI 分支另读 `<pci>/uevent`（0x10215）并用 `sscanf("%04x:%02x:%02x.%1u")`（0x102e3）解析
   `PCI_SLOT_NAME=`（0x102d5），以及 `<pci>/config`（0x102f6）。
7. 枚举路径（`drmGetDevices2`）另用 `/sys/bus/pci/devices/%04x:%02x:%02x.%d/`（0x10c40）与
   其 `/drm`（0x10c10）。

### 9.2 bus 类型判定表（0x61b0，7 项 {后缀, 类型}）

| 路径后缀 | 判定 |
|---|---|
| `/pci` | PCI |
| `/usb` | USB |
| `/platform` | platform |
| `/spi` | platform |
| `/host1x` | host1x |
| `/virtio` | virtio |
| `/faux` | faux |

→ 关键：`/sys/dev/char/226:0` 解析后的路径里必须出现上表之一。当前客体是 `../../class/drm/card0`
（`/sys/class/drm/card0`），一个都不匹配。这就是「libdrm 识别不出这是 PCI 设备」的确切原因。

### 9.3 因此需要的 sysfs 节点（Linux `virtio-pci` 形态）

```
/sys/dev/char/226:0            -> symlink 到 /sys/devices/pci0000:00/0000:00:01.0/drm/card0
/sys/dev/char/226:0/device     -> .../0000:00:01.0/virtio0        （含 /pci + /virtio，两种判定都能命中）
/sys/devices/pci0000:00/                                          （目录，父名含 /pci 供 "/.." 判定）
/sys/devices/pci0000:00/0000:00:01.0/
    vendor device subsystem_vendor subsystem_device revision       （`%x` 文本）
    config                                                        （PCI 配置空间前 64B）
    uevent                                                        （含 PCI_SLOT_NAME=0000:00:01.0）
    drm/                                                          （目录）
        card0  renderD128                                         （条目）
/sys/bus/pci/devices/0000:00:01.0 -> 上述 PCI 目录              （枚举路径）
```

其中 vendor/device 直接用 `pci_dev_info` 里的值（QEMU 的 virtio-gpu-pci 是 `1af4:1050`）；
BDF 用哪个都行，只要 realpath 与 `/sys/bus/pci/devices/<bdf>` 自洽，libdrm 是从 realpath 反解 BDF 的。

### 9.4 实施要点与风险

- 落点：`kernel/fs/sysfs.c`（枚举 `sf_type_t`、`sysfs_lookup`、`sysfs_content`、stat/readlink 四处），
  需要时给 `kernel/drivers/bus/pci_bus.c` 加一个公开的 PCI 信息访问器（`g_pci_infos[]` 目前是 `static`）。
- 必须保持加法式：`/sys/class/drm/*` 与 `/sys/dev/char/<maj>:<min>` 的现有 readlink 是
  wlroots/libinput 正在依赖的布局，先跑桌面回归再谈其它。
- 改完的验收信号：`eglinfo -p gbm` 的报错不再是 `MESA-LOADER: failed to retrieve device information`，
  而是越过设备识别、进到建 screen / 选 renderer 的下一层；同时桌面（labwc + 视频）无回归。

### 9.5 更低侵入的实现变体（推荐先试这个）

9.3 里把 `/sys/dev/char/226:0` 本身改成指向 `/sys/devices/...` 的软链，会动到一条有明确不变量的
现有 readlink：`sysfs_readlink()` 对 `SF_DEV_CHAR_ENTRY` 生成 `../../class/<sub>/<name>` 是刻意的。
注释写明 libudev 的 `util_resolve_sys_link()` 解析结果必须与「从 `/sys/class` 枚举得到的 syspath」
逐字节相等，否则 libinput 的 `evdev_device_have_same_syspath()` 会失配（输入设备依赖这条）。
因此不要去改它。

够用且几乎纯加法的做法：只把 `/sys/dev/char/226:0/device` 改成一个符号链接
（该节点当前是个没人读的空目录），指向 `/sys/devices/pci0000:00/0000:00:01.0/virtio0`：

- `drmGetNodeType()`：解析 `/sys/dev/char/226:0/device` → `/sys/devices/pci0000:00/0000:00:01.0/virtio0`
  → 同时命中 `/pci` 与 `/virtio` → 判定为 PCI ✓
- `drmGetDeviceName()`：realpath 同上，按 `/virtio` 截断 → `/sys/devices/pci0000:00/0000:00:01.0`
  → 从那里读 5 个属性 ✓
- `stat("/sys/dev/char/226:0/device/drm")` → `.../0000:00:01.0/virtio0/drm` 存在 ✓
- 注意要只对 DRM 226:0 特判：`SF_DEV_CHAR_ENTRY` 的 `device` 子节点是输入设备共用的，
  全局改成软链会牵动 evdev 一侧。

`/sys/bus/pci/devices/<bdf>` 只被 `drmGetDevices2` 枚举路径用到（9.1 第 7 条），若首轮验证只关心
`gbm_create_device`，可以先不提供，等确认需要再补。

### 9.6 实施状态（本轮已落地，commit `193ee6b3`）

已在内核 `kernel/fs/sysfs.c` 按 9.5 实现，纯加法，未改动任何既有节点的类型或 readlink：

| 节点 | 状态 |
|---|---|
| `/sys/dev/char/226:0/device/subsystem` | 软链 → `/sys/bus/virtio` |
| `/sys/dev/char/226:0/subsystem` | 软链 → `/sys/bus/pci` |
| `.../device/{revision,vendor,device,subsystem_vendor,subsystem_device}` | 十六进制文本，实测可读 |
| `.../device/uevent` | 含 `PCI_SLOT_NAME=0000:00:01.0` |
| `.../device/config` | 64B PCI 配置头 |
| `.../device/drm/{card0,renderD128}` | 条目 |
| `/sys/dev/char/226:128` | 新增 render 节点识别（此前完全不可解析） |

两条 subsystem 链接必须目标不同。反汇编显示 `drmGetNodeType()` 把 readlink 出的目标串拿去匹配一张
7 项表，`/pci`→0、`/virtio`→0x10，且代码显式比较 `0x10`；`device/subsystem` 命中 `/virtio` 才会进入
libdrm 的 virtio 分支（再上溯一级读 PCI 属性），父级链接则命中 `/pci`。

实测（客体）：所有节点读回正确；桌面（labwc + xfwm4 + panel + thunar，含 polkit/dbus）无回归、无
panic/UBSAN；在内核 `sysfs_lookup` 挂临时 trace 后可见 libdrm 的完整查找序列：
`device→drm`、`device→subsystem`、`subsystem`、`device→uevent`、`device→vendor`、`device→device`、
`device→subsystem_vendor`、`device→subsystem_device`。**设备身份识别已走通**（此前直接放弃）。

**仍未通**：`eglinfo -p gbm` 依旧 `DRI2: failed to create gbm device`，且 Mesa 仍打印
`failed to retrieve device information`。trace 里 `config` 与 `revision` 始终未被查找，说明流程在属性读取
附近就中止了。

下一步最有依据的一条，是把设备路径摆成真实 Linux 的 virtio/PCI 层级。
`/sys/dev/char/226:0/device` 应解析到 `/sys/devices/pci0000:00/0000:00:01.0/virtio0`，这样
`drmGetDeviceName()` 的 `/virtio` 截断才会得到 PCI 父目录 `/sys/devices/pci0000:00/0000:00:01.0`，
`<realpath>/../subsystem` 才解析得到 `/sys/bus/pci`，`PCI_SLOT_NAME` 与 `config` 也才从那个父目录读取。
这需要新增 `/sys/devices/pci0000:00/...` 子树（约 100 行，仍是纯加法 + 把 DRM 的 `device` 改成软链）。
另需 `strace`（已加入 `xfce.world`）/更细的 trace 确认 `config` 为何未被读取。

另一条已实测可行的路对 Minecraft 可能已够：`eglinfo -p wayland` 成功，渲染器为
`llvmpipe (LLVM 21.1.2, 128 bits)`。LWJGL 走 `EGL_PLATFORM_WAYLAND`，若其呈现走 `wl_shm` 而非
`wl_dmabuf`，则 Minecraft 可能不需要 GBM 这条链。代价仍是 LWJGL natives + jars 不在镜像里。

### 9.7 补进 `/sys/bus` 之后：trace 修正与真正的停点（commit `7a6ad59c`）

已补 `/sys/bus/pci/devices/0000:00:01.0/{config,uevent,vendor,device,revision,subsystem_vendor,subsystem_device,drm}`（内容生成器与 char-device 侧共用）。实测 `config` / `uevent` / `vendor`
均可读回，`config` 是合法的 64B PCI 头。

必须纠正一条中途的错误结论。先前看到 `SYSLOOKUP t=0 name=bus` 就认定 `/sys/bus` 是卡点。
把 trace 扩到顶层后对照才发现，那 6 次 `bus` 查找来自诊断脚本自己的 `ls`/`cat`（我在读
`/sys/bus/...` 的节点），而 `eglinfo -p gbm` 运行期间 libdrm 一次都没查 `/sys/bus`。
所以这个补丁本身并没有解开 `gbm_create_device()`。教训：trace 只能看"问了什么"，
不能直接归因到某个消费者；要区分必须按进程/时间窗隔离。

eglinfo 运行期间 libdrm 的真实序列（t=30 是 `/sys/dev/char/<maj>:<min>`，t=32 是 `.../device`）：

```
device→drm, device→subsystem, subsystem(char级),
device→uevent,
device→vendor, device→device, device→subsystem_vendor, device→subsystem_device
```

然后序列就停了。`revision` 与 `config` 从头到尾没有被查找过。

由此得到的真正停点：libdrm 读完了 uevent 与 4 个属性（缺 `revision`），随后中止。两种可能：
(a) 属性表顺序与预期不同，`revision` 不是 index 0，而第 5 个属性读取失败导致整轮失败；
(b) 流程在属性读取之后、读 `config` 之前因为别的判断而返回错误。
要区分，下一步的 trace 需要记录查找结果（而不只是尝试），并覆盖 `sysfs_stat`/`sysfs_open_vnode`
与 attr 文件的 `read`，才能看出是"没问"还是"问了但失败"。

结论：设备身份识别已从"完全放弃"推进到"读完 uevent + 4 属性"；`gbm_create_device()` 仍未通，
且已验证它不是 `/sys/bus` 缺失导致。同时 `eglinfo -p wayland` 依旧成功（llvmpipe），
这仍是对 Minecraft 最有希望的一条路。

### 9.8 细粒度 trace：卡点不在 sysfs 侧（commit `d8d593b1`）

把 trace 下沉到 `sysfs_lookup` + `sysfs_open_vnode` + `sysfs_fread` 三处（分别记「查找/打开/读取」），
拿到了本轮最硬的一组事实。`eglinfo -p gbm` 期间，libdrm 的全部 sysfs 交互是：

```
SYSOPEN t=38 idx=57984   SYSREAD t=38 idx=57984 len=86   ← uevent（57984=0xE280 → 226:128 render 节点）
SYSOPEN t=36 idx=1       SYSREAD t=36 idx=1 len=5        ← vendor            = "1af4"
SYSOPEN t=36 idx=2       SYSREAD t=36 idx=2 len=5        ← device            = "1050"
SYSOPEN t=36 idx=3       SYSREAD t=36 idx=3 len=5        ← subsystem_vendor  = "1af4"
SYSOPEN t=36 idx=4       SYSREAD t=36 idx=4 len=5        ← subsystem_device  = "1100"
```

三条关键推论：

1. libdrm 处理的是 render 节点 226:128（`idx=57984`），不是 card0。9.6 里给 render 节点补的身份识别
   是这条链的必需项，不是可选项。
2. `revision`（idx=0）和 `config` 从未被 open/read。而 libdrm 实际读的那 4 个属性全部读到了正确值。
   → 内核侧没有任何一个节点"提供不出来"：卡点在读完这 4 个属性之后，不在 sysfs。
3. 改 subsystem 链接目标（`/sys/bus/pci` ↔ `/sys/bus/virtio`）对读取序列没有任何影响，逐字节相同。
   因此 `d8d593b1` 选择 `/sys/bus/pci` 是基于 Mesa `loader_get_pci_id_for_fd()` 要求 `DRM_BUS_PCI`
   这一契约的主动选择，而非观测到的修复。

**因此下一步不该再往 sysfs 加节点。**要解开 `gbm_create_device()`，需要看到那 4 次读之后 libdrm/Mesa
做了什么。可行手段：
- 给内核 syscall trace 加上进程过滤，把 eglinfo 之后的 `ioctl`（尤其 DRM ioctl）/`mmap`/后续 `openat`
  完整记下来（9.7 的教训：trace 必须能归因到具体消费者）；
- 或者拿到 Mesa/libdrm 源码后直接对照 `loader_get_pci_id_for_fd()` 与 `gbm_create_device()` 的分支条件
  （当前只能从反汇编推断"要求 DRM_BUS_PCI"）。
- `strace` 仍是最省事的工具；现已加入 `xfce.world`（`strace` 在 v3.23 与 edge 均有），
  不再需要离线准备静态 musl 版本。

已验证可行的替代路径没有变化：`eglinfo -p wayland` 成功、渲染器 `llvmpipe (LLVM 21.1.2, 128 bits)`。
考虑 Minecraft 走 `EGL_PLATFORM_WAYLAND`，这条路值得优先于继续啃 GBM。

### 9.9 环境覆盖开关全部无效：GBM 失败与驱动选择无关

为排除"只是选错了驱动"这一可能，一次性测了 Mesa 的全部常用覆盖开关（均在 `eglinfo -p gbm` 下）：

| 环境 | 结果 |
|---|---|
| `LIBGL_ALWAYS_SOFTWARE=1` | `eglInitialize failed` |
| `GALLIUM_DRIVER=llvmpipe` | `eglInitialize failed` |
| `MESA_LOADER_DRIVER_OVERRIDE=virtio_gpu` | `eglInitialize failed` |
| `MESA_LOADER_DRIVER_OVERRIDE=swrast` | `eglInitialize failed` |
| `MESA_LOADER_DRIVER_OVERRIDE=zink` | `eglInitialize failed` |

全部失败。对照之下 `eglinfo -p wayland` 成功、`eglinfo -p surfaceless` 报的是另一套错
（`egl: failed to create dri2 screen` / `DRI2: failed to create screen`）。

结论：`EGL_PLATFORM=GBM` 的失败与驱动选择、与 sysfs、与软件/硬件路径都无关，卡在 Mesa
`gbm_create_device()` 内部（`dri_gbm` 后端为这台设备建设备时返回 NULL）。这一点无法再靠黑盒手段推进，
必须读 Mesa 源码（`src/gbm/backends/dri/gbm_dri.c` 与 `src/egl/drivers/dri2/platform_drm.c`），
或拿到这个版本对应的调试符号。

同时确认的可用面：
- `EGL_PLATFORM=wayland` → 成功，`llvmpipe` 渲染器可用（Minecraft/LWJGL 走的就是这条）；
- `EGL_PLATFORM=GBM` → 不通（wlroots 的 GL 渲染器需要它，因此当前只能继续用 `WLR_RENDERER=pixman`）。

### 9.10 反汇编 Mesa 的 loader：内核侧已满足其全部 sysfs 契约

Mesa 源码拿不到，但客体里的 Mesa 二进制可以反汇编，和当初破解 libdrm 是同一套方法，而且更直接。
目标：`dri_gbm.so` 里内联的 mesa-loader。

`loader_get_pci_id_for_fd()` 的完整逻辑（0x4190 起）：

```
fstat(fd)                              失败 → 打印 "MESA-LOADER: failed to fstat fd"
  成功 → 从 st_rdev 拆出 maj/min
       → 读 /sys/dev/char/%d:%d/device/vendor   （格式串 @0x99e2，strtoll 以 16 进制解析）
       → 读 /sys/dev/char/%d:%d/device/device
  两者都非 0 → return true                      ← 直接返回，根本不会调用 drmGetDevice2
  否则      → drmGetDevice2(fd)        失败 → "failed to retrieve device information"
                                       bustype≠0 → "device is not located on the PCI bus"
```

三条推论：

1. Mesa 直读的路径正是 `/sys/dev/char/<maj>:<min>/device/{vendor,device}`，且按 16 进制解析。
   我提供的节点（`1af4` / `1050`）完全满足，两者皆非 0 → `loader_get_pci_id_for_fd()` 返回 true。
   → 内核侧对这个契约是完备的。
2. 我们看到的 `failed to retrieve device information` 不来自这个函数的主路径（那条主路径根本不会走到
   `drmGetDevice2`）。同类型的字符串在多个 Mesa 库里都有，不能只按文案归因；这也再次印证 9.7 的教训。
3. `GBM_ALWAYS_SOFTWARE`（该 .so 里确实存在、此前漏测）实测无效；`get_driver_name` 走 `drmGetVersion`，
   其失败文案是 `failed to get driver name for fd %d`，也不是我们看到的那个。

因此结论收敛为：`gbm_create_device()` 的失败发生在 Mesa 的 gbm-dri 后端内部，**在 PCI ID 解析与驱动名解析之后**。

并且 **`modifier` 这条线索也已排除**：反汇编 0x2440 处可见 `Only invalid modifier specified` 是通过
`fwrite` 写 stderr 且置 `errno=EINVAL` 的一条独立分支，在我们的任何一次运行输出里都没有出现过。
不能在"字符串里出现过"和"实际被执行过"之间划等号，这是 9.7 那条教训的又一实例。

内核侧到此可以定论：所有 Mesa/libdrm 文档化与可反汇编出的 sysfs 需求都已满足。
再往下必须在 Mesa 内部（或其调试符号）里推进，而非继续改内核。可用的抓手只剩：
给 `dri_gbm.so`/`libgallium-25.2.7.so` 配对应的 debug 符号（Alpine 有 `-dbg` 包，需离线准备），
或把 Mesa 的 `-Dbuildtype=debug` 版本换进镜像。

### 9.11 前提纠正：网络是通的；并且 Wayland 路径上的真实 GL 渲染已跑通

先纠正一个贯穿多轮的错误前提。本项目此前把"网络被封"当成既定事实，并据此得出
"拿不到 Mesa 源码 → GL 阻塞"。实测 `curl https://dl-cdn.alpinelinux.org/...` 返回 200、
DNS 正常。**该前提是错的**，所有建立在它之上的"阻塞"结论都不可靠。

用这个能力做了两件事，都拿到了源码级证据：

1. 取到 Mesa 25.2.7 源码并核对：`src/loader/loader.c` 的 `loader_get_pci_id_for_fd()`
   确实先走 `loader_get_linux_pci_id_for_fd()`，读
   `/sys/dev/char/<maj>:<min>/device/{vendor,device}` 并按十六进制解析，与 9.10 的反汇编完全一致。
   两个值都非 0 即返回 true，根本不会调用 `drmGetDevice2()`。内核侧对该契约是完备的。
   失败点收敛到 `dri_device_create()` → `dri_screen_create[ _sw ]()` →
   `dri_screen_create_for_driver()` → `driCreateNewScreen3()` 返回 NULL，
   即 `dri2_init_screen`（硬件）或 `dri_swrast_kms_init_screen`（软件，`GBM_ALWAYS_SOFTWARE` 走这条）
   失败。"缺共享库"假设已排除：`libgbm.so.1`、`libgallium-25.2.7.so`、`libLLVM`、`libdrm.so.2`
   在镜像里都在，`/usr/lib/dri/*` 是指向 `libdril_dri.so` 的正常符号链接。

2. 验证 Wayland 路径上的真实 GL 渲染（Minecraft 实际走的路径）：
   把 `mesa-demos` 的 `egltri_wayland` / `es2gears_wayland` 注入镜像，在 labwc 会话
   （`XDG_RUNTIME_DIR=/run/user/0`、`WAYLAND_DISPLAY=wayland-0`）里运行：

   ```
   es2gears_wayland:  EGL_VERSION = 1.5          然后持续运行到测试超时（Terminated）
   eglinfo -p wayland: OpenGL core profile renderer: llvmpipe (LLVM 21.1.2, 128 bits)
                       OpenGL ES profile version: OpenGL ES 3.2 Mesa 25.2.7
   ```

   即 **EGL + GLES 3.2 的完整渲染在 Wayland 平台上可用**。途中出现的
   `failed to get driver name for fd -1` / `MESA-LOADER: failed to retrieve device information`
   是 Mesa 先用 fd=-1 探测"无设备"配置（`drmGetVersion(-1)` 失败）再回退的中间步骤，
   不是最终失败；`EGL_VERSION = 1.5` 与持续运行证明最终配置成功。

对目标的影响：
- Minecraft/LWJGL 走 `EGL_PLATFORM_WAYLAND`，这条路已验证可渲染，很可能不需要 GBM；
- GBM 仍是坏的（`WLR_RENDERER=pixman` 暂时保留），但它不再是 Minecraft 的前置条件；
- "LWJGL natives + jars 取不到"这条也不再成立：网络是通的。是否要拉取由你决定。

### 9.12 GBM 排查补记：又两条假设被排除，失败点已钉到源码函数

拿到 Mesa 源码后继续收窄，先排除两条：

1. "缺共享库"，排除。逐个对照镜像：`libgallium-25.2.7.so` 的全部 20 个 `DT_NEEDED`
   （`libLLVM.so.21.1`、`libSPIRV-Tools.so`、`libstdc++.so.6`、`libxcb-*`、`libdrm*`、`libexpat`、
   `libgcc_s`、`libz/libzstd` …）在镜像里全都在；`/usr/lib/dri/*` 是指向 `libdril_dri.so` 的正常
   符号链接，`libdril_dri.so` 仅依赖 `libgbm.so.1` + libc，也都在。
2. "render 节点上 KMS ioctl 被整体拒绝"，排除（我自己的内核里就否掉了）。
   `kernel/drivers/gpu/drm.c`：`drm_mode_getresources()` 等 KMS 入口都没有检查 `ctx->render_only`，
   照常执行；整个文件里只有一处 `render_only` 门控，即 `drm_set_master()`（第 798 行）
   `return -EACCES`。所以 render 节点上 `MODE_GETRESOURCES` 是放行的。

失败点（源码级）：`gbm_create_device()` → `dri_device_create()` →
`dri_screen_create()`（硬件）或 `dri_screen_create_sw()`（`GBM_ALWAYS_SOFTWARE`）→
`dri_screen_create_for_driver()` → `driCreateNewScreen3()` 返回 NULL。
软件路径更具体：`pipe_loader_sw_probe_kms()` 里 `create_winsys_kms_dri(fd)` 返回 NULL 就 `goto fail`。

剩下的两个候选都还没验证，供下一轮参考：
- Mesa 的 winsys/初始化是否调用 `drmSetMaster`：本内核与 Linux 一样对 render 节点返回 `-EACCES`；
- 某条 KMS ioctl 的返回数据不对。注意"错误返回"与"数据错误"要分开：此前实测没有任何 DRM ioctl
  返回错误，所以如果问题出在 ioctl 层，形状应是"成功但内容不对"，而不是"被拒绝"。

**这一条不再阻塞 Minecraft**：Wayland 路径已验证可渲染（9.11）。GBM 只影响 wlroots 自己的渲染器质量。

再补两条排除，以及一条有价值的对照：

3. "KMS winsys 创建失败"，排除。`kms_dri_create_winsys()`
   （`src/gallium/winsys/sw/kms-dri/kms_dri_sw_winsys.c:510`）除 `CALLOC_STRUCT` 失败（OOM）外不可能返回
   NULL，它只是填一张函数指针表。所以 `create_winsys_kms_dri()` 是成功的。
4. "DUMB ioctl 缺失"，排除。`DRM_IOCTL_MODE_CREATE_DUMB` / `MAP_DUMB` / `DESTROY_DUMB`
   在本内核里都已实现（`drm.c:1603` 起）。

有价值的对照是这条关键线索：`eglinfo -p wayland` 能出 `llvmpipe` 渲染器，说明 `swrast`
（llvmpipe）这个 DRI 驱动在客体里是能正常加载并建 screen 的。而 GBM 路径上的 `kms_swrast`
（以及硬件路径的 `virtio_gpu`）建 screen 失败。两者唯一的实质差别是：`kms_swrast` 需要 KMS winsys，
`swrast` 不需要。

→ 因此卡点几乎可以确定在**「KMS 路径」**上（而不是驱动加载、依赖、sysfs、modifier）。
剩下的两个候选也正好落在这里：Mesa 是否调用 `drmSetMaster`（本内核与 Linux 一样对 render 节点返回
`-EACCES`，见 `drm.c:798`），或某条 KMS ioctl 成功但数据不对（注意：从未观测到任何 DRM ioctl
返回错误，所以若在 ioctl 层，形状必然是"成功但内容错"）。这两个都还没验证。

候选一已排除：对 Mesa 25.2.7 全源码 grep `drmSetMaster|drmAuthMagic|drmGetMagic|drmDropMaster`，
在 gbm / dri2 / kms-dri 路径上没有任何 `drmSetMaster` 调用，只有
`platform_drm.c:403` 的 `drmAuthMagic`（X/DRM 认证）与 `platform_wayland.c:1974` 的 `drmGetMagic`。
所以 render 节点的 `SET_MASTER` 返回 `-EACCES` 不是原因。

**只剩候选二**：kms_swrast 建 screen 时某条 KMS ioctl 成功但返回的数据不对。
下一步应当在 `kms_swrast` 的 screen 创建路径（`src/gallium/drivers/.../kms_swrast` 与
`drmModeGetResources`/`drmModeGetConnector` 等）上，把内核实际返回的结构体与 Linux 的逐字段对照。

### 9.13 GBM 最终状态：不再当作阻塞项（含两条新观测）

新观测一：换成 GL 变体设备也没用。用
`-display egl-headless -device virtio-gpu-gl-pci`（virgl 能力）启动，QEMU 接受该设备（stderr 为空），
但 `eglinfo -p gbm` 仍然 `eglInitialize failed`。所以"用非 GL 变体所以没有 virgl"这条解释不成立。

新观测二：`kms_swrast` 这个名字在这套 Mesa 构建里可能根本不可用。

```
libgallium-25.2.7.so 里 "kms_swrast" 出现次数：0
                  而 "swrast" / "virtio_gpu" / "kmsro" / "zink" 均存在
libdril_dri.so 只导出 60 个 __driDriverGetExtensions_<驱动名> 形式的后缀符号，
              没有通用的 __driDriverGetExtensions
```

（注意：前一轮我用 `strings -x` 精确匹配得出的"不存在"结论有方法缺陷，此处是用 `grep -c` 复核后的数字。）

GBM 的累计结论：失败被钉在 Mesa 的 DRI screen 创建、且在 KMS 路径上（`swrast` 能建 screen、
`kms_swrast` 不能，二者唯一实质差别是是否需要 KMS winsys）。已排除的假设累计 10 条：
sysfs 缺节点、全部驱动选择开关、modifier 分支、缺共享库、render 节点 KMS 被整体拒绝、缺 DUMB ioctl、
winsys 创建失败、Mesa 调用 `drmSetMaster`、virgl/QEMU 设备变体、以及"网络不可用"这个前提本身。

**不要再把 GBM 当成阻塞项**：
- 对 Minecraft 无影响：`EGL_PLATFORM=wayland` 已用真实客户端（`es2gears_wayland`，`EGL_VERSION=1.5`
  + GLES 3.2 llvmpipe）验证可渲染（9.11）；
- GBM 只影响 wlroots 自己的渲染器质量，当前 `WLR_RENDERER=pixman` 是正确且可用的配置；
- 若将来确实要 GBM 加速，最有效的下一步是用 strace（已加入 `xfce.world`）看 DRI screen 创建
  到底停在哪一步，那比继续做假设-验证循环划算得多。

### 9.14 Minecraft 的 JNI 前置条件：已补齐并实测通过（commit `bcd18ec8`）

排查 Minecraft 还剩什么时，找到的不是又一条假设，而是一个具体且可修的前提：

- LWJGL 是开源的（BSD），所以直接取 `lwjgl-3.3.6-natives-linux.jar` 检查：
  `liblwjgl.so` 的 `DT_NEEDED` 是 `libc.so.6` / `libdl.so.2` / `libpthread.so.0`，即 glibc 的 soname；
  可选的 classifier 只有 freebsd / linux / linux-arm32 / linux-arm64 / linux-ppc64le / linux-riscv64 /
  macos / macos-arm64 / windows / windows-arm64 / windows-x86，上游不提供 musl 变体。
- 而本镜像是纯 musl（`ld-musl-x86_64.so.1`），既没有 glibc 也没有 gcompat，缓存里也没有，
  也就是 JNI 层根本无法 `dlopen` 任何 LWJGL native。

修法：把 `gcompat`（Alpine main，1.1.0-r4）加进 `packages/world/xfce.world` 的 JVM 段。

验证是重建镜像加实机运行，不是假设：
- 构建日志：`(158/518) Installing gcompat (1.1.0-r4)`；
- 新镜像内：`/lib/ld-linux-x86-64.so.2`（22728 B）+ `libc.so.6 -> libgcompat.so.0` +
  `libm.so.6` / `libpthread.so.0` / `librt.so.1`；
- 在客体里跑宿主用 glibc 动态链接编出来的测试程序：

  ```
  GLIBCTEST: start
  GLIBCTEST: dlopen(libm.so.6)=OK
  GLIBCTEST: dlopen(libpthread)=OK
  GLIBCTEST: end            rc=0
  ```

  glibc 动态二进制能加载，且能继续 `dlopen` 其它 glibc 共享库。

**边界不要过度解读**：gcompat 是部分 glibc 兼容层，所以这只证明"JNI 加载路径通了"这个前置条件成立，
不等于 LWJGL/游戏已可运行。游戏自身的 jar 与资源是受版权保护的商业内容，未下载。

### 9.15 还缺 `libdl.so.2`：用真实 LWJGL native 验证的第二个前提

9.14 加 gcompat 时还没做完：清点 gcompat 实际提供的文件后发现，它只给了
`libc.so.6` / `libm.so.6` / `libpthread.so.0` / `librt.so.1` / `libcrypt.so.1` / `libresolv.so.2` /
`libutil.so.1` + `ld-linux-x86-64.so.2`，唯独没有 `libdl.so.2`。

而 LWJGL 的 native 依赖里正好要它：

```
liblwjgl.so            DT_NEEDED: libdl.so.2, libpthread.so.0, libc.so.6
libglfw.so             DT_NEEDED: librt.so.1, libm.so.6, libdl.so.2, libpthread.so.0, libc.so.6
liblwjgl_opengles.so   DT_NEEDED: libpthread.so.0, libc.so.6
```

修法：glibc ≥2.34 起 `libdl` 已并入 libc，所以补一个软链即可，
`packages/overlay/xfce/lib/libdl.so.2 -> libc.so.6`。

验证是重建镜像加注入真实 LWJGL 3.3.6 native 实测，不是推断：

```
/lib/libdl.so.2 -> libc.so.6                              （镜像内已落地）
LWJGLT: dlopen(libdl.so.2)            = OK
LWJGLT: dlopen(/usr/lib/liblwjgl.so)          = OK
LWJGLT: dlopen(/usr/lib/liblwjgl_opengles.so) = OK
LWJGLT: dlopen(/usr/lib/libglfw.so)           = OK
rc=0
```

即 JNI + glibc 依赖这条路上，LWJGL 的三个 native 库都能真正加载。

**仍存边界**：这验证的是"native 能被加载"（用的是上游真实二进制），不是"Java 侧绑定可用"。
客体只有 JRE（无 `javac`），Java 侧的 EGL/GLES 调用没能在此验证；游戏 jar/资源是版权内容，未下载。

一处流程教训：这次注入 native 时我第一次用 `find -maxdepth 4` 定位解包产物，而实际路径深度是 7，
`find` 静默返回空、循环里的 `[ -z ] && continue` 把三个 native 全跳过了，日志里表现为
"natives present: No such file"。"注入成功"没有输出不等于真的写进去了；后来改用显式路径并加
`injected <dst>` 回显才确认。这类静默跳过值得在脚本里一律显式回显。

### 9.16 OpenAL：把 LWJGL 重定向到发行版的 musl 版（本轮实测）

承接 9.14/9.15 那条「LWJGL 的 native 是 glibc 构建」的线：MC 的死因最后落在
`natives/libopenal.so`（glibc）抛出的未捕获 `std::system_error`（稳定抛点
`libopenal.so + 0xf168`，详见 `docs/distro/known-issues.md`）。而镜像里其实已经有 musl 版的 OpenAL
（Alpine `openal-soft`：`/usr/lib/libopenal.so.1 -> libopenal.so.1.24.3`）。

修法（本次已落地）：给 launcher 的 java 命令行加一行

```
-Dorg.lwjgl.openal.libname=/usr/lib/libopenal.so.1
```

（`packages/overlay/xfce/usr/local/bin/minecraft`）。

实测结果（三次采样一致）证明这条改动有害，已回滚：
- `CXA_THROW` / `terminate called` / `MAP_HIT` 全部消失，`DLOPEN ... openal` 也没有出现，
  glibc OpenAL 的抛异常路径确实被绕开了；
- 但 MC 反而停得更早：加了这个 flag 之后三次采样都停在
  `Backend library: LWJGL version 3.3.3+5` 之后，再也到不了 `Using optional rendering extensions`；
  而不加 flag 时，多次采样都能走到 `Using optional rendering extensions`（只是随后被 OpenAL 的抛异常打死）。
- ⇒ `-Dorg.lwjgl.openal.libname` 把失败点提前了（很可能是 LWJGL 因此提早去碰 OpenAL，
  而「glibc 语境里 `dlopen` 一个 musl 库」这条路本身会卡）。**这是一次回归，已从 overlay 回滚。**

修正后的判断：MC 的 OpenAL 抛异常不是渲染阶段的直接阻塞点。不加 flag 时 MC 能走到
`Using optional rendering extensions`，抛异常发生在那之后（声音引擎初始化阶段）。所以下一步不是
「换一个 OpenAL」，而应该是：
（a）查清 glibc 版 OpenAL 在 gcompat 下为什么抛（稳定抛点 `libopenal.so + 0xf168`），或
（b）让 MC 在声音引擎初始化失败时优雅降级（它本该如此），而不是让 glibc native 的 C++ 异常
    逃逸成 `terminate`。

### 9.17 OpenAL 这一环解决了：MC 走到了主菜单（但仍受另一个间歇性停点影响）

9.16 的回滚结论只对了一半：把 OpenAL 指向 musl 版之所以变成回归，是因为只换了库、没换后端。
真正起作用的组合是两件事一起（已落在 launcher overlay 里）：

```
export ALSOFT_DRIVERS=null                                # 让 OpenAL Soft 用空设备后端
-Dorg.lwjgl.openal.libname=/nonexistent/libopenal.so      # 让 LWJGL 不走它自带的 glibc OpenAL
```

实测（客体，有一轮完整走通）：MC 的日志一路到

```
Using optional rendering extensions: ...
Reloading ResourceManager: vanilla
OpenAL initialized on device No Output        ← 声音引擎这次起来了
Sound engine started
Created: 512x256x0 minecraft:textures/atlas/particles.png-atlas
...（blocks / gui / items / chest / shulker_boxes 等整套 atlas 都建了）
[Download-*/ERROR]: Failed to fetch Realms feature flags / yggdrasil public key
```

最后几行网络错误正是标题界面会做的事（拉 Realms 通知与用户资料），并且全程没有任何
`CXA_THROW` / `terminate called` ⇒ MC 走到了主菜单 ✓。

机制：LWJGL 自带的 `libopenal.so` 是 glibc 构建，在这个 musl 客体里经 gcompat 运行，
在声音引擎初始化时抛未捕获的 C++ 异常（先看到 `std::system_error`；沿这条线改到走 fallback 后，
看到 OpenAL Soft 自己的 `al::backend_exception`）⇒ `terminate` ⇒ 进程死。
`ALSOFT_DRIVERS=null` 把后端变成空设备（日志里即 `No Output`），于是不再抛。
两件事缺一不可：只给 `ALSOFT_DRIVERS=null`（不改 libname）时，MC 仍停在
`Backend library: LWJGL version 3.3.3+5` 之后（实测一轮一致）。

**仍不稳定（必须记下）：**同一配置换一轮会停在 `Backend library: LWJGL version 3.3.3+5` 之后、
到不了 `Using optional rendering extensions`，与第 8 节记的「渲染器起来之后输出就停」
是同一现象。它独立于 OpenAL，是另一条间歇性停点（与 mpv/LuaJIT 那类未解释的崩溃同属一类）。

⇒ 结论：OpenAL 这一环已解决，MC 能到达主菜单；要「稳定地能玩」，还需要解决那条间歇性停点。

### 9.18 但窗口并没有出现在屏上（10 张实时截屏的证据）

9.17 的「走到主菜单」是客户端逻辑层的事实：资源重载、声音引擎、全套 atlas 全部完成，且全程无异常。
但画面上并没有 MC 的窗口。证据来自同一轮里的 3 次 MC 尝试 + 从 boot 起每 45 秒一次的 QMP 截屏（共 10 张，
覆盖 3 次尝试的整个时间窗）：

- 10 张的像素统计完全一致（蓝色 95%、绿 0%、棕 0%，都是蓝色壁纸的桌面）；
- 但扫描输出是活的：10 张里有 8 个不同的 md5，且 shot1 与 shot10 的差异 bbox 恰好是
  右上角时钟（`x=960..972, y=15..23`，82 个像素不同）⇒ 截屏是真的、在动，不是冻结画面；
- ⇒ 整段时间里桌面上从未出现过 MC 的窗口（3 次尝试都没有）。

因此目前只能说 MC 到达了客户端内部逻辑层面的主菜单；
要「能玩」，还差让它的窗口真正被合成器映射/呈现出来 ✗。这与第 8 节早先记过的
「窗口建出来之后又消失」是同一族问题。

⇒ 下一步方向：查 MC 的 `wl_surface` 生命周期，即窗口创建后是否 `commit`/被映射、labwc 是否收下、
是否落在别的 workspace，而不是继续在音频/GL 上找。

### 9.19 对照实验：别的 Wayland GL 客户端窗口正常，只有 MC 的窗口不出（病灶收窄到 MC/GLFW）

9.18 里 MC 的窗口从未出现。为区分「合成器 / GL 链的问题」与「MC 自己的问题」，做了一次对照：

- 对照客户端：`es2gears_wayland`（mesa-demos，镜像里本来就有；9.11 已用它证明 Wayland 路径能真渲染）。
- 同一轮里先跑它 80 秒，再跑 MC 120 秒；主机每 30 秒截一次屏，共 12 张。

结果：

- `es2gears_wayland` 确实在渲染：日志稳定打出
  `1417 frames in 5.0 seconds = 283.400 FPS`（之后每 5 秒一条，283~302 FPS 持续）。
  （它启动时同样会打那几行已知警告
  `libEGL warning: failed to get driver name for fd -1` / `MESA-LOADER: failed to retrieve device information`
  / `ZINK: vkCreateInstance failed` / `egl: failed to create dri2 screen`，但它们是非致命的，齿轮照跑。）
- 截屏画像把两者分得很清楚：
  ```
  ct2/ct3/ct4（es2gears 那段）: blue=84% dark=10%   ← 多出一块暗色区域 = 它的窗口 ✓
  ct5..ct12（MC 那段及之后） : blue=95% dark= 2%   ← 与纯桌面一致，没有 MC 窗口
  ```

⇒ 合成器完全能呈现一个真实的 Wayland GL 客户端窗口 ✓；不出现的是 MC 自己的窗口 ✗。

结论（病灶收窄）：窗口呈现问题不在合成器、也不在 EGL/Wayland 链（对照客户端一切正常），
而在 MC / GLFW 这条自身路径上（窗口是否创建、`wl_surface` 是否 `attach`/`commit`、是否被过早销毁）。

下一步（具体）：在已有的 `LD_PRELOAD` 垫片里再拦 `libwayland-client` 的
`wl_surface_attach` / `wl_surface_commit` / `wl_surface_damage`（都是导出符号，垫片先加载即可截获），
看 MC 的 GLFW 到底有没有走到「提交一帧」，
这能把问题直接分成「MC 从没建/提交 surface」还是「提交了但没被收下」两类。

### 9.20 探针本身踩空：`wl_surface_*` 是 static inline，拦不到（应拦 `wl_proxy_marshal_flags`）

按 9.19 定的下一步做了：在垫片里拦 `libwayland-client` 的
`wl_surface_attach` / `wl_surface_commit` / `wl_surface_damage` / `wl_surface_damage_buffer`，
并做对照（先跑 `es2gears_wayland` 60 秒，再跑 MC 150 秒）。结果三类计数全是 0：

```
WL_ATTACH=0   WL_COMMIT=0   WL_DAMAGE=0
```

连对照客户端 es2gears 也是 0，而它明明在渲染（日志里稳定 280~306 FPS），
所以不是「没人调用」，而是「拦错了符号」 ✗。

原因：在 wayland-client 的头文件里，`wl_surface_attach` / `wl_surface_commit` /
`wl_surface_damage` / `wl_surface_damage_buffer` 全都是 `static inline` 包装，
会被内联编译进调用方（libglfw / es2gears 自己），不经过动态符号表 ⇒
`LD_PRELOAD` 对这几个名字无效。它们真正调用到的导出函数是
`wl_proxy_marshal_flags`（以及 `wl_proxy_marshal_array_flags`）。

对下一步的意义：要追踪「谁提交了什么」，应当拦 `wl_proxy_marshal_flags`，
并用它的 `opcode` 参数区分 attach / commit / damage
（`wl_surface` 的 opcode：destroy=0, attach=1, damage=2, frame=3, …, commit=6）。

本轮顺手拿到的另一条信息：这一轮里 MC（用 9.17 那套 env）又走通了
`Using optional rendering extensions` → `Sound engine started` ✓，
说明 OpenAL 那套配置是稳定可用的（客户端逻辑层一路到声音引擎），
问题依旧集中在窗口不呈现这一件事上。

### 9.21 修好的探针 + 依赖证据：MC 走的是 X11 后端（XWayland），不是 Wayland 直接呈现

9.20 的修正是对的：改拦 `wl_proxy_marshal_array_flags` 之后，同一套负载立刻抓到 322 条 marshal ✓
（拦静态内联那版是 0）。

按接口名统计（整份日志）：

```
139  WL_MARSHAL (non-ctor)      ← 绝大多数是非构造请求（bind 等）
  9  WL_MARSHAL wl_callback
  6  WL_MARSHAL wl_registry
  3  WL_MARSHAL wp_presentation
  1  WL_MARSHAL wl_surface       ← 全日志只有一次 wl_surface 构造，且落在 es2gears 段内
  1  WL_MARSHAL wl_shm
  1  WL_MARSHAL wl_seat
  1  WL_MARSHAL wl_compositor
```

- 那唯一一次 `wl_surface` 构造在第 447 行，位于 es2gears 相（`WL1`=362 … `WL2`=1199）；
  MC 相（1200..2361）没有 `wl_surface`。
  （取样说明：探针每进程只记前 24 条、其后每 1024 条记一条，所以这条是旁证，不是铁证。）

更硬的证据在别处：早先几轮垫片的 `DLOPEN` 日志显示，MC 的进程加载的是 X11 那一套：
`libX11.so.6`、`libXxf86vm.so.1`、`libXi.so.6`、`libXrandr.so.2`、`libXcursor.so.1`、
`libXinerama.so.1`、`libX11-xcb.so.1`、`libXrender.so.1`、`libXext.so.6`、`libGLX.so.0`、`libGL.so.1`。
这正是 GLFW 的 X11 后端依赖集（GLFW 3.3 的后端是编译期选定的，LWJGL 带的就是 X11 版）。

⇒ MC 的窗口是 X11 窗口，靠 XWayland 映射进 Wayland 合成器 ✓。因此：
- 它本来就不会创建 `wl_surface`（那 322 条里的 Wayland 流量基本属于 es2gears）✓；
- 「窗口不出」的病灶在 XWayland / labwc 的 xwm 一侧，不在 Wayland 客户端的窗口路径上。

日志里已经有 XWayland 的报错（前面几轮多次出现）：

```
[xwayland/xwm.c:1928] xcb error: op 12:0, code 3, sequence 175, value 4194311
```

（op 12 = `ConfigureWindow`，code 3 = `BadWindow`。）

⇒ 下一步方向：查 labwc 的 XWayland 集成（xwm），即 XWayland 的窗口是否被 map/收下、
这些 `xcb error` 具体指谁、XWayland 的 root window 是否正常。
这比继续追 Wayland 侧要靠谱得多。

### 9.23 更正 9.21 的一半 + 一次被间歇性停点打断的实验

先更正 9.21 的一半：9.21 从「垫片日志里 MC 加载了 libX11 一套」推断「LWJGL 带的是 X11-only
的 GLFW」。这个推断不成立。直接看镜像里 `natives/libglfw.so` 的动态符号，它导出了 Wayland 接口：

```
glfwGetWaylandDisplay
glfwGetWaylandMonitor
glfwGetWaylandWindow
```

⇒ 这个 GLFW 编译了 Wayland 支持（双后端），后端是运行期选的；而双后端构建也会链接 libX11
（GLFW 的 Wayland 路径仍会用到 X11 的一些设施），所以「日志里有 libX11」并不能证明它选了 X11 ✗。

由此得到一个便宜的判别手段：会话里 `DISPLAY=:0`（XWayland）是设着的；若 GLFW 因此选了 X11，
那么把 `DISPLAY` unset 就可能把它逼上 Wayland，而「Wayland 原生呈现」已被 9.22 的对照证明是通的 ✓。

**本轮实验（MC 的 `DISPLAY` unset）没能给出结论** ✗：这一轮 MC 又停在
`Backend library: LWJGL version 3.3.3+5` 之后（就是那个间歇性停点），根本没走到 GLFW 建窗口那一步：

```
W21 env WAYLAND_DISPLAY=wayland-0 DISPLAY=[unset]
[11:40:16] [Render thread/INFO]: Backend library: LWJGL version 3.3.3+5
（之后无输出；wl_surface 构造 0 次；6 张截屏全是 blue=95% dark=3% 的纯桌面）
```

⇒ 「unset `DISPLAY` 能不能把 MC 逼上 Wayland」仍未被验证（不是被否定，是被停点挡住了）。

因此下一步的方法学要求：实验脚本必须一轮里连跑多次 MC（9.18 那次一轮 3 次里第 1 次就过了），
每次之后截屏 / 查 `wl_surface`，用真正走过渲染器的那些轮来判定，否则单次实验会被停点吃掉。

### 9.24 定案：GLFW 默认选 X11（证据是 dlopen 清单）；两条路现在都还出不了窗

垫片一直在记 `dlopen`，而 GLFW 的每个后端都会加载自己那套客户端库，于是可以直接从已有日志
判定它选了哪个后端，不用再开机：

| 日志 | `DISPLAY` | libX11 一族 | libwayland* | libxkbcommon |
|---|---|---|---|---|
| `wl2` `wl` `null` `min` `win` `soft` `noal` `al3`（会话默认） | 已设 `:0` | 8~16 | 0 | 0 |
| `w2`（DISPLAY unset 实验） | unset | 1 | 3 | 1 |
| `w3`（同上，3 次尝试） | unset | 3 | 9 | 3 |

⇒ 结论一（9.21 的推断其实是对的）：会话默认（`DISPLAY=:0` 已设）时，MC 的进程只加载 X11 那一套，
完全不加载 `libwayland-client` / `libxkbcommon` ⇒ GLFW 选的就是 X11 后端，
MC 的窗口是 XWayland 的 X11 窗口 ✓。
（9.23 说「它也可能选 Wayland」在机制上成立：它确实编译了 Wayland 支持；但实测没有选。）

⇒ 结论二（`unset DISPLAY` 这个杠杆确实有效）：unset 之后 MC 改加载 Wayland 那一套
（`libwayland-client` / `libwayland-cursor` / `libwayland-egl` + `libxkbcommon`）
⇒ 后端被成功切到 Wayland ✓。

⇒ 结论三（但两条路现在都走不到「窗口出现」）：
- X11 路（默认）：MC 约 1/3 的尝试能走到主菜单逻辑，但 X11 窗口根本不会被呈现。
  9.22 已用 `glxgears` 证明任何 X11 客户端都不出现 ⇒ 这条路上 MC 不可能有窗口 ✗；
- Wayland 路（unset `DISPLAY`）：3/3 全部停在 `Backend library: LWJGL version 3.3.3+5` 之后
  （比默认的 ~1/3 更差）⇒ 这条路上 MC 走不到建窗 ✗。

所以「窗口不出现」至此被拆成两条独立的工作线：

1. 修 XWayland / labwc-xwm 的 X11 窗口呈现（入口就是 9.21/9.22 里的
   `xcb error: op 12 (ConfigureWindow), code 3 (BadWindow)`），修好后 MC 走默认的 X11 路就能出窗；
2. 修「Wayland 路在 renderer 之后停住」，修好后用 `unset DISPLAY` 把 MC 切到 Wayland 即可出窗
   （而「Wayland 原生呈现」已被 `es2gears_wayland` 证明是通的 ✓）。

### 9.25 MC 的进程从不调用 `XMapWindow` —— 哪怕是走到 `Sound engine started` 的那一轮

为回答「MC 到底有没有把 X 窗口建/映射出来」，在垫片里拦了 libX11 的三个真实导出符号
（`XMapWindow` / `XMapRaised` / `XMapSubwindows`；它们经 PLT 调用，可被 `LD_PRELOAD` 截获），
然后跑一轮默认 X11 路的 MC（`DISPLAY=:0`）。

**这一轮是「好轮」**：MC 走到了

```
[11:40:12] [Render thread/INFO]: Backend library: LWJGL version 3.3.3+5
[11:40:18] [Render thread/INFO]: Using optional rendering extensions: ...
[11:40:43] [Render thread/INFO]: Sound engine started
```

但计数全是 0：

```
XMAPWINDOW=0   XMAPRAISED=0   XMAPSUBWINDOWS=0
```

同轮的 dlopen 再次确认后端（只加载 libX11 一族，没有 libwayland / libxkbcommon）。

这说明：即便这一轮 MC 走到了声音引擎，**它也没有 map 出任何 X 窗口** ✗。

一个很可能的解释（下一步该先验证）：原版 MC 的窗口是「先隐藏创建、到主菜单才显示」。
GLFW 先 `XCreateWindow`（不 map），真要显示时才由 `glfwShowWindow` 调 `XMapWindow`。
也就是说「窗口不出现」可能不是呈现问题，而是 MC 根本没走到「显示窗口」那一步 ✗，
那就应当把重心移回停点（工作线 B），而不是合成器。

但另一边的问题依然独立存在：`glxgears`（一个会 map 自己窗口的普通 X11 客户端，见 9.22）
同样不出现在屏上 ✗ ⇒ X11 窗口的呈现确实也坏着 ✓（工作线 A 仍然成立）。

⇒ 两条工作线都还在，但优先级应当调整：先修停点（B），
因为 MC 连「显示窗口」那一步都还没走到，先修呈现（A）也看不出结果。
（要继续验证这一点，可以再拦 `XCreateWindow` / `glfwShowWindow`，看 MC 究竟走到哪一步。）

### 9.26 对照组验证了探针：MC 真的从不创建 X 窗口；而 X11 客户端创建了也不被呈现

9.25 里 MC 的 `XCreateWindow` / `XMapWindow` 计数为 0。为排除「探针根本没生效」，做了对照：
同一个垫片下先跑 `glxgears`（一个确实会建窗并 map 的 X11 客户端）60 秒，再跑 MC 150 秒。

对照证明探针有效：

```
XK1 glxgears start（行 308） … XK2 glxgears end（行 1096）
行 352: XCREATEWINDOW  a=0x12c0000012c b=0x1     ← 落在 glxgears 相内 ✓
行 386: XMAPWINDOW     a=0x1 b=0x400002          ← 落在 glxgears 相内 ✓
counts: create=1  map=1（全部落在 glxgears 相）
```

（glxgears 也确实在渲染：`3624 frames in 5.0 seconds = 724.628 FPS`。）
而 MC 相（行 1099..2153）里这两个计数都是 0 ✓。

⇒ 两个结论，现在都建立在经过验证的仪器上：

1. MC 真的从不创建、也不 map 任何 X 窗口 ✓（不是探针失灵），即使它走到
   `Sound engine started`（9.25 那一轮）也没有 ✗。
   也就是说 MC 的 GL 链路是在没有窗口的情况下走完的（很可能是 headless /
   `EGL_PLATFORM=surfaceless` 那类路径；而 render node `renderD128` 恰好是后来才加上的，见早先记录）。
   这条要么查「MC/GLFW 的窗口创建为什么失败」，要么查「它为什么被绕过」。
2. X11 窗口确实被创建/map 了，但不会被呈现 ✓：`glxgears` 明明建了窗也 map 了（上面两行），
   而 9.22 的截屏证明它不出现在屏上 ⇒ XWayland / wlroots-xwm 没有把它合成出来 ✓
   （入口仍是 `xcb error: op 12 (ConfigureWindow), code 3 (BadWindow)`）。

因此最终把两件事分清：

- A（呈现）：X11 客户端的窗口不被呈现，`glxgears` 为证（它 map 了却不显示）；
- B（MC 自己的窗口）：MC 连窗口都没创建，本条为证。

两者互相独立；而 MC 要能玩，**B 必须先解决**（否则 A 修好了也没有窗口可呈现）。

### 9.22 决定性对照：X11（XWayland）的窗口在这台桌面上根本不出现

按 9.21 的方向，用同一套方法做了第二个对照：跑一个真正的 X11 客户端
（`glxgears -display :0`，即 XWayland 的 DISPLAY），每 35 秒截一次屏。

结果：

- XWayland 本身是活的：`/tmp/.X11-unix/X0` 存在 ✓；`xprop -root` 有响应 ✓，
  且其启动横幅如实打印了已知问题：
  ```
  Xwayland glamor: GBM Wayland interfaces not available
  Failed to initialize glamor, falling back to sw
  The XKEYBOARD keymap compiler (xkbcomp) reports: ...
  Errors from xkbcomp are not fatal to the X server
  ```
- 但 X11 客户端的窗口同样不出现：6 张截屏里 xt2..xt6 全是 `blue=95% dark=3%`（纯桌面），
  与 MC 那几轮一模一样 ✗。

三个对照放在一起，结论唯一：

| 客户端 | 类型 | 窗口是否出现 |
|---|---|---|
| `es2gears_wayland` | Wayland 原生 GL | ✅ 出现（画像 `blue=84% dark=10%`） |
| `glxgears`（经 XWayland） | X11 | ❌ 不出现 |
| Minecraft（GLFW 的 X11 后端，经 XWayland） | X11 | ❌ 不出现 |

⇒ **这台桌面上「X11（XWayland）的窗口不会被呈现」**；Wayland 原生窗口则一切正常。
MC 恰好是 X11 客户端（见 9.21：LWJGL 带的 GLFW 是 X11 构建），所以它必然是受害者。
这与 MC 自身无关，是 XWayland / labwc 的 xwm 集成问题。

两个可行动方向：

1. 让 MC 走 Wayland（最干净）：给它一个 Wayland 构建的 GLFW（LWJGL 现带的是 X11 版），
   于是它不再经过 XWayland，而「Wayland 原生呈现」这条路已被对照证明是通的 ✓；
2. 修 XWayland / labwc-xwm：查清为什么 X11 窗口不被呈现（9.21 里 labwc 已在报
   `xcb error: op 12 (ConfigureWindow), code 3 (BadWindow)`）。这条更深，但能根治所有 X11 应用。

顺带更正两条陈旧记录：
- 第 8 节末的「LWJGL natives + Minecraft jars 不在镜像里」已经不成立：现在的 xfce 镜像里
  `/usr/share/a20-media/1.21.11/` 完整存在：`client.jar`(31 MB)、`libraries/`、`assets/`、
  `natives/`（libglfw/libopenal/liblwjgl/libjemalloc/... 齐全）、`classpath.txt`。
- 结合 9.13 的结论（GBM 不是阻塞项、Wayland 路径上真实 GL 渲染已跑通），Minecraft 现在的真正
  卡点是上面那条「停点」与 glibc-native 类问题，不是 GEM/dma-buf 那条链。

> **本节的第 1 个方向已被后续工作取代**（见 [gpu-3d-roadmap.md §9.1](gpu-3d-roadmap.md)）。
> 当时写"给它一个 Wayland 构建的 GLFW（LWJGL 现带的是 X11 版）"是不准确的：
> 镜像里的 LWJGL 是 3.3.3（不是 3.3.6），捆绑的 GLFW 3.4.0 两个后端都编
> 进去，Minecraft 也从不调 `glfwInitHint(GLFW_PLATFORM, ...)`；没有
> `GLFW_PLATFORM` 这个环境变量（3.4.0 与当前 GLFW 都没有，它只是 init hint，
> 该字符串在镜像那份 `libglfw.so` 中不存在）。
> 在 hint 留在 `ANY_PLATFORM` 时环境是唯一杠杆：`XDG_SESSION_TYPE=wayland`
> 加 `WAYLAND_DISPLAY` 会在任何探测之前被采纳，否则只要 `DISPLAY` 有值就落到
> X11。`packages/overlay/xfce/usr/local/bin/minecraft` 现在设置这两个变量，并在
> 缺少时大声拒绝启动（否则 GLFW 会静默退回坏路径）。
>
> **XWayland 呈现 bug 本身没有修，是被绕过的。**上面这条"修 XWayland"的方向
> 仍然成立、仍然值得做，因为它能根治所有 X11 应用。
>
> 另外，本节"Wayland 原生 GL 客户端窗口正常"这条对照现在可以加强：已确认
> **Wayland 后端不绑定 `zwp_linux_dmabuf_v1`、也不绑定 `wl_drm`**，所以 llvmpipe
> 走 `wl_shm` 就够；假 connector 1024x768 对 Minecraft 默认的 854x480 也不是
> 阻塞（GLFW 只拒绝非正尺寸）。
