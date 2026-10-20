# 发行版与引导当前状态

> 仍未完成的事项见 [1.0 缺口台账](../gaps/README.md)；本页只描述当前状态。

发行版 rootfs 与引导链的当前进展。

<!-- 以下内容从 docs/distro/boot.md 迁入；只保留当前状态，历史运行记录已删除。 -->

## 实测走到哪一步

riscv64 QEMU 下（已提交状态）整条链路是通的：合成器不崩溃、DRM 后端创建成功、`Virtual-1` modeset 到 1024x768、`WAYLAND_DISPLAY=wayland-0` 起来、autostart 把 XFCE 组件拉起来，`wlr_surface` 陆续映射，`[DRM] present` 持续出帧。输入也已打通：eudev 把 `ID_INPUT_*` 写进数据库，libinput 枚举到 event0/event1，labwc 把它们配置成键盘鼠标。剩下的已知小问题（dbus 同步调用偶发超时）见 [dbus 诊断](../gaps/notes/desktop-dbus-credentials.md)。
