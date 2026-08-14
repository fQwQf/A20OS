# 从 A20OS init 到 XFCE 桌面：一条启动链路

发行版模式下，A20OS 的启动其实分成两段：前半段是内核和 A20OS 自己的 init， 负责把发行版挂起来并交权；后半段完全属于发行版，A20OS 不再插手。下面顺着链路走一遍。

## 第一段：内核挂载 + A20OS init 交权

内核在 `kernel/fs/mount_setup.c` 里按 `FINAL_ROOT_PATH`（开发构建是 `/test`， 决赛是 `/mnt`）自动挂好这几样：

```
/test/dev       devtmpfs —— 实际上是 A20OS 的 devfs：/dev/dri/card0、/dev/input/event0 都在这里
/test/dev/shm   tmpfs
/test/proc      proc
/test/sys       sysfs  —— A20OS 精简 sysfs：/sys/class、/sys/dev、/sys/devices/virtual
/test/run       tmpfs
```

`user/init.c` 的 `detect_distro_rootfs()` 检查两个条件，同时满足才进发行版模式：

- `/test/etc/a20-distro` 存在（overlay 里放的标记文件）；
- `/test/sbin/init` 存在且是常规文件。

于是 `enter_distro_rootfs()` 执行 `chdir("/test"); chroot("/test"); execve("/sbin/init")`。 A20OS 自己那份用户态到此让位，发行版的 `/sbin/init`（软链到 overlay 里的 `usr/lib/a20/init`）成为 PID 1。这之后 A20OS 只做一件事：当一个"Linux 兼容内核"。

## 第二段：stage-2 init（overlay `/usr/lib/a20/init`）

这是 A20OS 写进发行版的编排脚本，顺序上先把基础运行时准备好，再逐个拉起服务：

1. **/dev 与运行时目录**。A20OS 的 devfs 已经填好了 /dev，脚本只在缺 `/dev/console` 时才重挂 devtmpfs；接着挂 tmpfs 到 `/dev/shm`、devpts 到 `/dev/pts`，建好 `/run`、`/run/user/0`、`/tmp/.X11-unix` 等。
2. **补缓存**。因为 apk 的 post-install 不跑，这里补 gdk-pixbuf loader 和 mime 缓存（见 build.md 第 2 步）。
3. **dbus**。`dbus-uuidgen --ensure` 之后 `dbus-daemon --system --fork`。系统总线 是一切服务的基础，后面 elogind/polkit 都靠它。
4. **elogind**。XFCE 的 session manager 和 polkit 要跟它上面的 org.freedesktop.login1 接口说话。elogind 想挂 cgroup（/sys/fs/cgroup），A20OS 现在没有 cgroup 子树，这一步会报失败，但当前不致命，桌面试过能继续走。
5. **seatd**。`SEATD_VTBOUND=0 seatd &`，然后等 `/run/seatd.sock` 出现。libseat 是 labwc/wlroots 拿 session 的入口。
6. **udevd**。`udevd &` → 等 `/run/udev/control` → `udevadm trigger --action=add` → `timeout 5 udevadm settle`。udev 在 A20OS 上属于"能用但不完整"：netlink uevent 已经打通（见 kernel-requirements.md），但规则引擎的运行期处理还有问题 （见 known-issues.md）。所以脚本里所有等 udev 的地方都带超时，绝不让一个卡住的 udevd 挡住桌面会话。
7. **polkitd**。`/usr/lib/polkit-1/polkitd &`，给 XFCE 的一些辅助程序做授权。
8. 最后 `exec runuser -l root -c /usr/lib/a20/start-xfce4-session` 交到会话。

## 第三段：XFCE Wayland 会话（overlay `/usr/lib/a20/start-xfce4-session`）

脚本把环境变量摆好，关键渲染配置是：

- `WLR_DRM_DEVICES=/dev/dri/card0` —— A20OS 没有 udev 数据库来描述显卡， wlroots 直接按环境变量打开 KMS 设备；
- `WLR_DRM_NO_ATOMIC=1` —— 走 legacy DRM 接口；
- `WLR_RENDERER=pixman`、`WLR_NO_HARDWARE_CURSORS=1` —— 没有 virgl 用户态， 纯软件渲染，TCG 模拟下也撑得住；
- `WLR_LIBINPUT_NO_DEVICES=1` —— 输入还没打通之前的临时开关（见 known-issues）。

然后 `exec dbus-run-session -- labwc`。labwc 起来后读 `root/.config/labwc/autostart`， 把 xfsettingsd、xfdesktop、xfce4-panel、thunar 依次拉起，连上 Wayland 显示开始渲染。

## 实测走到哪一步

riscv64 QEMU 下（已提交状态）整条链路是通的：合成器不崩溃、DRM 后端创建成功、 `Virtual-1` modeset 到 1024x768、`WAYLAND_DISPLAY=wayland-0` 起来、autostart 把 XFCE 组件拉起来，`wlr_surface` 陆续映射，`[DRM] present` 持续出帧。剩下没走通的 是输入（libinput 枚举不到设备）和偶发死锁，都记在 known-issues.md。
