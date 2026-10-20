# 桌面壁纸不显示（backdrop 不渲染）

> 来源：原 `docs/status/current/known-issues.md` 的对应小节；该文件已拆分删除。
> 关联台账：[DESKTOP-09](../desktop.md)。

xfdesktop 在跑（桌面图标正常、root 被背景层覆盖），桌面却是纯黑：1024x768 下 94% 像素为 `(0,0,0)`。

第一轮把 key 匹配排除掉了。`/backdrop/screen0/monitor<id>/workspace0/last-image` 的 `<id>` 用的是
`sha1(connector)`，而 `sha1("Virtual-1")` 正好在 overlay 的 9 个 monitor key 里；`image-style=5`、
`color-style=0` 合法；改成 SVG 壁纸并显式加 `image-show=true` 后仍然全黑。顺带发现镜像里没有
jpeg/png 的 gdk-pixbuf loader（`loaders.cache` 有 ani/bmp/gif/icns/ico/pnm/qtif/svg/tga/tiff/xbm/xpm），
`gdk-pixbuf` 也不依赖 libjpeg/libpng，所以 `xfce-blue.jpg` 本来就无法解码，这也是已改指向
`xfce-flower.svg` 的原因。

第二轮转到 guest 内实测。`xfconf-query -c xfce4-desktop -lv` 里 backdrop 树完全正确：
`monitor0/workspace0` 与 9 个 `monitor<sha1(connector)>/workspace0` 全是 `image-show=true` /
`image-style=5` / `last-image=/usr/share/backgrounds/xfce/xfce-flower.svg` / `color-style=0`；SVG 文件
确实存在（16972 B），`libpixbufloader_svg.so` 在 loaders.cache 里注册了，`librsvg-2.so.2`(2.61.2)
也在镜像里。解码链（gdk-pixbuf → svg loader → librsvg）看起来应该可用，`last-image` 指向的文件也在，
画面却仍全黑，于是第二个原因只能在渲染侧：xfdesktop 的 backdrop 窗口（layer-shell 表面）到
labwc(WLR_RENDERER=pixman) 合成这一段，或者 xfdesktop 对 SVG 的 rasterize 失败。

第三轮二分 decode 与 render：把壁纸换成一个 `loaders.cache` 一定支持的 BMP（宿主造了张纯红
64×64 BMP，`debugfs` 注入 `/root/red.bmp`，两个 monitor key 都改成它并 restart xfdesktop）。
截图里红色像素为 0，画面还是纯黑。连 BMP 都不渲染，问题就不在解码，而在「backdrop 上图 / 合成」
这一段。同一轮还出现了 `SIGSEGV: pid=163 code=13 stval=0x12`（用户态空指针附近访问），很可能是
xfdesktop/缩略图相关组件在画 backdrop 时挂掉。下一步看渲染侧：`xfdesktop` 加
`--enable-debug`/`G_MESSAGES_DEBUG=all` 看 backdrop 的加载与绘制，确认 backdrop 的 layer-shell 表面
是否真的 `commit`（labwc 日志 / `wayland-info`），并查那个 `stval=0x12` 的 SIGSEGV 属于哪个进程。

第四轮做 xfdesktop 全量 debug。`G_MESSAGES_DEBUG=all xfdesktop` 输出里没有任何
backdrop/image/draw 相关日志，只有 dconf/GTK/GIO 的常规初始化
（`Compositor prefers decoration mode 'server'`、`Connecting to session manager`、
没有 session manager/portal、`mnt_monitor_get_fd failed: Operation not permitted`）。
也就是说 xfdesktop 根本没走到「加载并绘制 backdrop」（或那一段无日志），
backdrop 表面上没有被真正画出来。

第五轮查 xfdesktop 版本与协议。镜像是 xfdesktop 4.20.1，链接了 `libgtk-layer-shell.so.0`；二进制里有
`Your compositor must support the zwlr_layer_shell_v1 protocol` 这条串，说明它的 Wayland backdrop 依赖
wlr-layer-shell。但日志里并没有这条报错，也没有任何 backdrop 行；同时 `xfdesktop --version` 正常、
xfce4-panel 正常。收敛判断是 xfdesktop 4.20.1 的 Wayland backdrop 很可能根本没被绘制（该版本 Wayland
支持较新且有缺口），而不是协议缺失或解码失败。可行修法：改用合成器层面的背景工具（`swaybg`/`wbg`
之类，仅需一张图并挂 background 层），或在 xfdesktop 里确认 Wayland backdrop 的开关/补丁。镜像里目前
没有 swaybg/wbg/hsetroot/feh 任何一个。

第六轮按上一轮的修法把 `swaybg`(1.2.1) 加进 `xfce` world 并启动（当时放在
`start-desktop-components.sh` 里，现已移到 labwc 的 `autostart`，理由见前面
「udevd 在 bring-up 阶段崩溃会让桌面黑屏」一节），重制镜像后验证。swaybg 确实起来了并配置成功
（`[main.c:282] Found config * for output Virtual-1 (AOS A20OS Display 0x00000001)`，
进程存活 12s+ 未退出），但截图里依然没有背景：

  - `swaybg -i /usr/share/backgrounds/xfce/xfce-flower.svg -m fill` → 桌面区仍是纯黑；
  - `swaybg -m solid_color -c 00aa22`（纯绿）→ 绿色像素 0；
  - `pkill -x xfdesktop` 之后再截图 → 仍是纯黑（排除「xfdesktop 用黑表面盖住背景层」）。

两种模式（图片 / 纯色）都失败，说明不是解码、也不是模式选择，而是 labwc 没有把 background 层的
wlr-layer-shell 表面合成进 scanout；同一会话里 panel 所在的层却正常渲染。收敛到这里，壁纸真正的
阻塞点是 labwc / wlr-layer-shell 的 background 层合成路径，不是「缺一个背景工具」，也不是 xfdesktop
的 backdrop 开关（第四、五轮的结论需要按此修正）。下一步：在 labwc 侧看 background 层表面的
map/commit 与 pixman renderer 的 damage 处理，单独排除 `WLR_RENDERER=pixman`、
`WLR_NO_HARDWARE_CURSORS=1`、`WLR_DRM_NO_ATOMIC=1` 三个开关。

第七轮换时间维度采样，结论是第六轮错了。第六轮的截图都是在启动 ~60s 之后拍的，而壁纸在那之前就
已经画出来过又被盖掉。用户观察「刚启动有壁纸，过一会儿变黑」后按时间重采（headless，每 5s 一张，
统计桌面区非黑像素）：

  | t(s) | 颜色数 | 最大占比色 | 黑像素 |
  |---|---|---|---|
  | 5–20 | 1–2 | (0,0,0) | ~100% |
  | **25** | **202** | **(0,144,188)** | **0.0%** |
  | 30+ | 26 | (0,0,0) | 99.4% |

swaybg 的壁纸确实画上去了（t=25s），5 秒后就被纯黑盖掉。
决定性实验：把 `spawn xfdesktop` 换成一句 echo（不启动 xfdesktop）后重采，壁纸从 t=25s 一直保持到
t=110s（202 色 / 0% 黑）。凶手就是 xfdesktop：它不画配置的 backdrop，而是铺了一块**不透明的黑色
桌面表面**，把 background 层上的壁纸整个盖住。第六轮「`pkill -x xfdesktop` 之后仍是纯黑」之所以
误导，是因为那时黑面已经合成进 scanout，杀掉进程并不会让合成器回到上一帧。

修法已验证：xfdesktop 留着（桌面图标照常），但让 swaybg 在它之后再创建自己的 background 表面，
wlr-layer-shell 同一层内后创建的表面在上面。实测「等 xfdesktop 进程出现 + settle 20s 再起
swaybg」：壁纸从 t=54s 稳定保持到 t=144s（229 色 / 0% 黑），面板与图标同时在。代价是启动后约 30s
内桌面是黑的；若日后能把 xfdesktop 的 Wayland backdrop 修好、或让它别铺那块黑面，这段等待就可以去掉。

顺带：同一批次里 `tumblerd` 以 `code=1`(#GP, `insn 0f b6 48 ..`) 崩溃多次（`FATAL: pid=... comm=tumblerd`），
是独立的用户态坏指针崩溃。同一轮还抓到一个确定的用户态崩溃：`FATAL: pid=140 ... comm=tumblerd`，
内核侧是 `ADE/ALE: pid=140 sepc=0x40226031 stval=0x8136a00e code=1`，又是 #GP（`code=1`，
x86_64 上只由 #GP 产生；`stval` 对 #GP 是过期 CR2），`insn@sepc=0x48b60ff4`（`0f b6 48 ..` 一个字节 load）。
即缩略图守护进程 tumblerd 在解引用一个坏地址而挂（xfdesktop 用它给桌面图标出缩略图）。这与 JVM
最初那条 #GP 同类（非规范地址/坏指针），是另一个独立的用户态崩溃，值得单独查。
