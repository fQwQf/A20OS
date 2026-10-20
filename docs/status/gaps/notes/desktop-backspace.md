# Backspace 失效（故障在合成器/终端一层）

> 来源：原 `docs/status/current/known-issues.md` 的对应小节；该文件已拆分删除。
> 关联台账：[DESKTOP-11](../desktop.md)。

## 四、Backspace 失效（实测结论：内核输入路径已排除，故障在合成器/终端一层）

XFCE 桌面里 `xfce4-terminal` 按 Backspace 什么都不发生，连 `^H`/`^?` 之类的可见字符都没有。
而内核输入路径是对的，这是本节的关键结论，且是实测不是推断。在 labwc 启动之前把
`/dev/input/event0` 的原始流 dump 到串口（此时还没有第二个消费者，避开了 input_mux「单消费者」
的抢占），按 a、b、Backspace、c、d 后把 24 字节的 `input_event` 记录逐条解码：

  ```
  01 00 | 1e 00 | 01 00 00 00    EV_KEY code=30 (KEY_A)         press
  00 00 | 00 00 | 00 00 00 00    EV_SYN
  01 00 | 1e 00 | 00 00 00 00    EV_KEY code=30                 release
  ...
  01 00 | 0e 00 | 01 00 00 00    EV_KEY code=14 (KEY_BACKSPACE) press
  00 00 | 00 00 | 00 00 00 00    EV_SYN
  01 00 | 0e 00 | 00 00 00 00    EV_KEY code=14                 release
  ```

时间戳是正常的 CLOCK_MONOTONIC，每个事件都配了 `EV_SYN`。即 `ps2.a20drv`（set-1 scancode
`0x0e` → evdev `14`）→ `input_mux` → `/dev/input/event0` 整条链全部正确。

端到端复现同样清楚：在终端里注入 `echo ab<Backspace>c<Enter>`，屏幕上是 `a20os# echo abc`
且输出 `abc`。letters/space/Enter 都到位，`b` 没被删掉，说明那个字节根本没到 tty；
否则 `stty erase = ^?` 的行规一定会删掉它。pty 侧的 ERASE 是实现了的：
`kernel/drivers/char/pty.c` 的 `pty_input_byte_locked()` 里 `ch == c_cc[PTY_CC_VERASE]`
会 `canon_len--` 并按 `ECHOE` 回写 `"\b \b"`；客体里 `stty -a` 报 `erase = ^?`。

客体 xkb 数据也是完整的：`keycodes/evdev` 有 `<BKSP> = 22;`、`symbols/pc` 有
`key <BKSP> {[ BackSpace, BackSpace ]};`、`rules/evdev` 存在；labwc 日志打
`[../src/config/keybind.c:136] Found layout English (US)`，说明布局解析成功。已排除的是
labwc 自己吞键（`root/.config/labwc/rc.xml` 只绑了 `W-Return`/`W-d`/`W-f`/`W-q`/
`W-Escape`，没有 BackSpace），以及 `xkbcomp` 的
`Unsupported maximum keycode 708, clipping` 警告，那是正常现象（`keycodes/evdev`
本就声明到 708 而文件头 `maximum = 255`，任何发行版都有这条）。

判别实验（已做）：在非 VTE 的 Wayland 客户端里试同一个键，`xfce4-appfinder`
的搜索框里打 `ab<Backspace>c`，框里显示 `ac`（`b` 被删掉了）；而同一段按键序列在
`xfce4-terminal` 里留下 `abc`。**即 libinput/labwc/xkb keymap 都是好的，问题只在 VTE。**

根因已定位并修复：`kernel/drivers/char/pty.c` 的 `pty_slave_ioctl()` 实现了
`TCGETS`/`TCSETS`，但 `pty_master_ioctl()` 没有，会走到 `return -ENOTTY`。Linux 上 pty
两端共享同一份 termios，而 VTE 系终端（xfce4-terminal）正是在 master fd 上调
`tcgetattr()` 来解析它 `auto` 的 Backspace 绑定；拿到 `ENOTTY` 就学不到 erase 字符
（客体里其实是 `^?`），于是 Backspace 一个字节都不发，与「字母/回车正常、只有 Backspace
毫无反应」完全吻合。已修：给 `pty_master_ioctl()` 补上 `TCGETS`/`TCSETS`/`TCSETSW`/`TCSETSF`，
与 slave 共用 `g_ptys[idx].termios`。实测同一段按键序列从 `a20os# echo abc`（`abc`）变成
`a20os# echo ac`（`ac`）。

顺带纠正一条：先前怀疑的 `input_mux` 伪造 `EVIOCGBIT(EV_KEY)`（`k = 1..0xff`）不是本例的
原因，同一设备上非 VTE 客户端的 Backspace 正常。该伪造仍不干净，但与本 bug 无关。
