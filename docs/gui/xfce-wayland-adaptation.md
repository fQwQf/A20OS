# XFCE Wayland 适配：内核强化与 submodule 去修改化

## 背景

XFCE 4.20 Wayland 桌面（labwc/wlroots 合成器 + xfce4-panel/xfdesktop）的适配过程中， 曾以修改 submodule 源码（wlroots、labwc、gtk-layer-shell、libxfce4windowing、xfdesktop 等） 的方式绕过内核缺陷。本任务的目标：

1. 通过强化操作系统自身（内核 + OS 侧用户态源码）消除这些缺陷；
2. 撤销所有 submodule 源码修改，保持 submodule checkout 纯净；
3. 必须保留的少量用户态规避改为 OS 树内的构建期补丁（`user/wayland/patches/`）， 构建时临时应用、构建后自动还原，不污染 submodule。

## 已完成的内核强化

### 1. DRM 页翻转事件语义（`kernel/drivers/gpu/drm.c`）

原实现只有单个 32 字节事件槽：连续两次 PAGE_FLIP 会覆盖前一个未读事件； sequence 恒为 0、时间戳全零；阻塞读完全不可用；无 EBUSY 语义。 事件丢失会永久卡死 wlroots 的帧调度（frame_pending 无法清除，新窗口永不显示， 直到鼠标输入凑巧触发其它路径）。

现实现：
- 每 fd 一个 32 字节固定记录的事件 FIFO（16 槽），事件绝不覆盖；
- 单调递增 vblank sequence + 单调时钟时间戳（timekeeping_get_monotonic）；
- 一个 pending flip 在完成前再次 PAGE_FLIP 返回 -EBUSY（与真实硬件一致）；
- 阻塞 read() 支持（park/wake 协议，可被信号打断），一次可读出多个事件；
- close() 时清理指向该 fd 的 pending flip（防悬垂）；
- vblank 状态机初始化使用两阶段 CAS（初始化完成的 READY 标记），消除 SMP 竞态。

### 2. 虚拟显示器 EDID（`kernel/drivers/gpu/drm.c`、`virtio_gpu.c`、`driver_class.h`）

wlroots 通过 libdisplay-info 解析 EDID 得到输出 make/model，GTK 依赖它构造 GdkMonitor；无 EDID 时 make/model 为 NULL。原方案是改 wlroots 硬编码默认值。

现实现：
- `virtio_gpu` 协商 `VIRTIO_GPU_F_EDID` 后执行 `VIRTIO_GPU_CMD_GET_EDID` （走大命令路径），缓存 128 字节基块；
- `gpu_dev_ops_t` 新增 `get_edid` 操作（virtio-gpu 实现，vmsvga 保持 NULL）；
- DRM 层暴露连接器 EDID 属性（blob property，id 1）：`GETCONNECTOR` 属性数组、 `GETPROPERTY`（名 "EDID"，BLOB|IMMUTABLE）、`OBJ_GETPROPERTIES` （含 `DRM_MODE_OBJECT_ANY` 查询）、`GETPROPBLOB`；
- 设备无有效 EDID（校验头+校验和）时内核合成一份完全合规的 EDID （"AOS"/"A20OS Display"，含 DTD、监视器名描述符与正确校验和）。

### 3. 标准 evdev 路径 `/dev/input/event0`（`kernel/fs/devfs/devfs.c`）

libseat/seatd 只接受 `/dev/input/event*` 作为输入设备，原系统只有 `/dev/event0`， 导致 wlroots 必须走 libinput path 后端（原 wlroots 补丁）绕过 udev/seatd。 现 devfs 增加 `input` 目录并在其中暴露同一个 evdev 复用器； 根目录 `/dev/event0` 保留为兼容别名。这样原生 wlroots 的 libinput udev 后端可经 libseat 打开设备。

## 已完成的 OS 侧用户态适配（非 submodule）

### 4. stub libudev 输入枚举（`user/wayland/stub/udev.c`）

原 stub 只枚举 fb0（graphics），输入枚举为空，原生 libinput udev 后端 找不到任何设备。现增加：
- `/sys/devices/virtual/input/event0` 枚举条目（linked list 化）；
- 设备属性：`ID_INPUT`/`ID_INPUT_KEYBOARD`/`ID_INPUT_MOUSE`=1， `ID_SEAT`="seat0"（与嵌入式 seatd 的 seat 名一致，原 "seat1" 会导致 设备被 libinput 拒绝）；
- devnode 改为 `/dev/input/event0`。

### 5. 会话启动器（`user/cmds/core/wayland-session.c`）

- 删除 `A20OS_NO_UDEV`/`A20OS_LIBINPUT_DEVICE`/`A20OS_NO_SYSTEMD`/ `A20OS_NO_HEADLESS` 等私有环境变量；
- 用 labwc 原生开关 `LABWC_UPDATE_ACTIVATION_ENV=0` 替代 `A20OS_NO_SYSTEMD`（labwc 原生支持该环境变量，无需改源码）；
- `WESTON_LIBINPUT_DEVICE` 指向 `/dev/input/event0`。

### 6. 构建期补丁机制（`user/wayland/build.sh`）

`patches/<组件>-a20.patch` 构建期应用、构建后 `git apply -R` 还原， 保证 submodule checkout 始终干净。保留的三个补丁均为纯用户态问题， 与内核无关：
- `libxfce4windowing-a20.patch`：不绑定 `ext_workspace_manager_v1` （与捆绑 wlroots 0.19 的实现存在协议生命周期分歧），退回内置 dummy workspace manager；
- `xfdesktop-a20.patch`：跳过 XSMP 选项组（Wayland 构建下 libxfce4ui 未导出 `xfce_sm_client_get_option_group` 符号，链接需求）；
- `thunar-a20.patch`：`XDT_CHECK_LIBX11_REQUIRE` → `XDT_CHECK_LIBX11` （Wayland 构建）。

### 7. 其它

- `user/Makefile`：LVGL 编译增加 `-DLV_LVGL_H_INCLUDE_SIMPLE` （修复组件重组后 `lvgl/lvgl.h` 包含路径失效，desktop 桌面无法编译）。
- `user/wayland/install-image.sh`：`WESTON_LIBINPUT_DEVICE` 路径更新。

## 运行时验证结果（riscv64 QEMU）

- 原生 wlroots：DRM 模式设置成功、EDID 解析成功（输出名 "Virtual-1"， make/model "AOS"/"A20OS Display"，`wl_output.geometry`/`zxdg_output` description 正常下发）；
- 原生 libinput udev 后端经嵌入式 libseat/seatd 打开 `/dev/input/event0` 成功（"libinput successfully initialized"），键盘/鼠标可用；
- 原生 labwc：headless 后端创建与销毁不再阻塞；帧事件链完整 （commit → schedule_frame → frame → render → pageflip → vblank 事件 → frame callback 回客户端）；
- xfce4-panel 可在 Wayland 上创建 layer surface 并持续渲染 （attach/damage/frame/commit 循环正常，帧回调按时返回）；
- 所有 submodule 工作树干净（`git submodule foreach` 全零）。

## 未完成 / 已知问题

1. **xfdesktop 崩溃（用户态，非内核）** xfdesktop 启动约 40s 后在 `glib` 的 `g_datalist_id_dup_data` （gbitlock futex 等待）处以空指针崩溃（sepc 偏移 0x268e6， stval=0x10）。调用链为 `xfw_screen_get(gdk_screen_get_default())`， `gdk_screen_get_default()` 返回 NULL。根因在用户态 （GTK3 Wayland 下默认 GdkScreen 未就绪或 xfdesktop 调用时序）， 与内核无关，未在本次改动中处理。建议后续在 xfdesktop 侧加 gdk_screen 空指针防护或调整初始化时序。

2. **xfce4-panel-wrapper 路径错误（镜像配置）** panel 报 `Failed to spawn the xfce4-panel-wrapper: /home/fqwqf/A20OS/user/build/wayland/riscv64/sysroot/lib/...`—— 构建期绝对路径被写入了镜像配置，属于 install-image 打包问题， 需在安装脚本中改为镜像内相对路径。

3. **smoke 测试时间窗** `smoke_qemu_gui.py` 的 "non-blank framebuffer scanout" 阶段固定 15s； 在纯软件渲染 + 模拟 RISC-V 上 15s 通常不够 XFCE 完整起桌面， riscv64 上建议加大该窗口或改用 `--settle` 手动长测。 另外宿主 CPU 被并行任务占用时整个测试会显著变慢。

4. **vblank 事件节奏** 当前 flip 完成事件在 ioctl 内同步完成（与 virtio-gpu 同步命令模型一致）。 曾试验 60Hz 虚拟 vblank 节奏投递，在慢速模拟环境下观察到不稳定， 故保留同步投递（FIFO/序号/时间戳/EBUSY 语义均已具备）。 后续若要在真机/快速环境下获得精确 vsync 节奏，可将 `drm_mode_pageflip` 中的投递点改为按 `next_vblank_tick` 延迟。

5. **labwc CONFIGURE_TIMEOUT_MS** 恢复为上游 100ms。模拟环境慢速客户端可能超时（超时行为本身安全， 仅按当前几何继续）。若出现窗口定位抖动，可再评估。

## 如何验证

```sh
# 1. 构建内核（全部架构）
make all PYTHON=python3

# 2. 构建 Wayland/XFCE 用户态（组件级，submodule 不落脏）
user/wayland/build.sh riscv64          # 全量（stamp 增量）
user/wayland/build-compositor.sh riscv64 wlroots labwc   # 合成器

# 3. 运行 GUI smoke（riscv64）
make smoke-qemu-gui-riscv64 SMOKE_TIMEOUT=240 PYTHON=python3

# 4. 确认 submodule 全部干净
git submodule foreach 'git status --short | wc -l'
```

## 发行版 rootfs 运行（`make distro-run`）

> **发行版路径的完整文档见 [`docs/distro/`](../distro/README.md)**（构建 / 启动 / 内核依赖 / 已知问题）。本节保留历史简况与 DRM 修复细节。

单条命令构建内核 + 用 `apk.static` 拉取 Alpine 发行版（含 dbus/elogind/polkit/seatd/eudev + XFCE4 + mesa）为 ext4 根镜像，再以 GUI 显示启动 QEMU。A20OS init 检测到 `/extra/etc/a20-distro` 标记后 `chroot("/extra")` 并 `exec /sbin/init`，由发行版自身的 stage-2 init 编排服务与 XFCE Wayland 会话，内核只提供 Linux ABI + /dev + /proc + /sys。

```sh
# riscv64（默认）或 x86_64
make distro-run
make distro-run-riscv64
make distro-run-x86_64

# 无显示器环境可用 none 显示后端跑无头
make distro-run ARCH=x86_64 QEMU_GUI_DISPLAY=none
```

相关新增文件：
- `user/rootfs/alpine/`（build.sh / packages.txt / overlay，含 stage-2 init 编排）
- `user/init.c`（distro rootfs 检测 + chroot 进入）
- `tools/targets-rootfs.mk`（`make rootfs-alpine`）、`tools/targets-distro.mk`（`make distro-run`）

### 发行版路径修复的两个内核 DRM bug（`kernel/drivers/gpu/drm.c`）

1. **`drm_mode_get_plane` 结构体越界**：此前给该结构加了 3 个非 UAPI 字段（`possible_crtcs_mask`/`possible_clones_mask`/`type`），`copy_to_user` 按 44 字节写回，溢出 libdrm 栈上 32 字节的 `struct drm_mode_get_plane`，踩掉栈 canary，labwc 启动即在 `drmModeGetPlane` 触发 `__stack_chk_fail`（musl `a_crash`，空写 SIGSEGV）。已改回标准 32 字节 UAPI 结构，plane 的 type 改由属性暴露。
2. **KMS 对象 ID 冲突**：plane/crtc/connector/encoder 此前都用 id=1。wlroots 的 `get_drm_prop` 用 `DRM_MODE_OBJECT_ANY` 按 ID 反查对象属性，ID 冲突导致它查 plane 时拿到 connector 的 EDID，找不到 plane 的 `type`/`IN_FORMATS`，于是 `Failed to create DRM backend`。已为四类对象分配唯一 ID（connector=1/encoder=2/crtc=3/plane=4），`OBJ_GETPROPERTIES` 改为按唯一 ID 解析（同时兼容 ANY 与具体类型查询），并新增 plane 的 `type=PRIMARY` 属性。

修复后发行版路径实测（riscv64 QEMU）：合成器不再崩溃，DRM 后端创建成功，`Virtual-1` modeset 1024x768，`WAYLAND_DISPLAY=wayland-0` 起来，labwc autostart 拉起 XFCE 会话（Xfconf/xfsettingsd 激活，窗口 `wlr_surface` 陆续映射）。输入已打通：eudev 把 `ID_INPUT_*` 写进数据库，libinput 枚举到 event0/event1 并配置成键盘鼠标。

发行版路径的**已知问题与排查笔记**（含已解决的输入 ABI 缺口与读路径自死锁的 根因、遗留的 dbus 偶发超时、测试环境注意点）统一维护在 [`docs/distro/known-issues.md`](../distro/known-issues.md)，本文档不再重复。
