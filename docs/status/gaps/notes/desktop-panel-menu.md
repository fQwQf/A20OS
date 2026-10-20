# 面板下拉菜单项不启动应用

> 来源：原 `docs/status/current/known-issues.md` 的对应小节；该文件已拆分删除。
> 关联台账：[DESKTOP-10](../desktop.md)。

面板按钮能开菜单，但点下拉菜单项既不启动应用（无新 `xdg_surface`、无窗口），也不关菜单。

在 `e2fsck` 干净的 rootfs 上复验，指针移动与坐标都正常：悬停分类项会弹出子菜单，点击面板按钮会打印
`press on layer-(sub)surface`。这与上面已解决的「窗口控制键无效」是两个不同症状；窗口控制/聚焦/拖动
已由 labwc 默认绑定修复，本条的客户端菜单条目点击仍未定位。

已排除内核输入通路本身。`struct input_event` ABI、PS/2 按键解码、每包 `SYN_REPORT`、evdev 打戳、
内核与 vDSO 的 CLOCK_MONOTONIC 时基逐项核对一致；libinput 也枚举到了设备（`Adding A20OS evdev mux [0:0]`）。

是否打日志取决于命中节点的类型（只有层表面分支会打印），所以「落在子菜单上的按下不打印任何 labwc
日志」本身是正常的，不能当作事件没送达的证据。下一步应在 guest 内直接看 `wlr_scene_node_at` 的命中结果
与客户端实际收到的 `wl_pointer.button`，不要再从截图推断。labwc 会把每个执行到的 action 记进日志
（`[action.c] Handling action <n>: <Name>`），这是比截图可靠的观测手段。
