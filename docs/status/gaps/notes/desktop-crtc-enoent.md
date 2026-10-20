# x86_64 桌面间歇性 `Failed to set CRTC`（ENOENT）

> 来源：原 `docs/status/current/known-issues.md` 的对应小节；该文件已拆分删除。
> 关联台账：[DESKTOP-13](../desktop.md)。

wlroots legacy 后端反复报
`[backend/drm/legacy.c:122] connector Virtual-1: Failed to set CRTC: No such file or directory`，
桌面没有画面，而且是间歇性的：同一份配置有的运行一次都不报，有的报 20 次。

内核侧唯一来源是 `drm_mode_setcrtc()` 中 `drm_find_gem(ctx, c.fb_id)` 返回 NULL
→ `-ENOENT`（`drm.c` 内该函数仅此一处 `-ENOENT`）。逐一验证排除了四种可能：

- GEM 表耗尽：插桩统计整个运行期只有 2 次 `drm_gem_alloc`（`slot=0`），
  `DRM_MAX_GEMS 64` 远未用满，表满的打印一次都没出现；
- `MODE_DESTROY_DUMB` 释放了仍被帧缓冲引用的 GEM：插桩后该 ioctl 调用次数为 0；
- `GEM_CLOSE` 释放了仍被引用的 GEM（即本分支 GEM_CLOSE 改动的嫌疑）：插桩后同样为 0，
  该 ioctl 在这条链路上根本不被调用；把 GEM_CLOSE 改成 no-op 也不改变现象；
- 内核与用户态 `drm_mode_crtc` ABI 不一致：本以为内核 `struct drm_mode_crtc`
  （`set_connectors_ptr`/`count_connectors`/`mode_valid`/`mode`）与 Linux UAPI
  不同构会导致 `fb_id` 读偏。原先这里给的依据是错的：它引用
  `user/external/mlibc/sysdeps/managarm/generic/drm.cpp` 说是"走同一份 vendored
  定义"，但那个文件不在 A20OS 构建路径里。A20OS 用
  `tools/targets-mlibc.mk` 配置 mlibc 的 `sysdeps/a20`，而 `sysdeps/a20`
  不含任何 DRM 代码。真正的用户态是 Alpine 的 libdrm（stock apk，跑在
  Linux syscall ABI 上），其 `drm_mode_crtc` 就是 Linux UAPI 的那一份。
  结论侥幸成立，但依据已更正；现在这条 ABI 一致性由
  `tools/check-drm-abi.sh` 门禁按结构体布局逐字段核对，见
  [graphics/3d-graphics.md §8.1](../../../graphics/3d-graphics.md)。

影响面：这条同时卡住 XWayland 呈现验证与 x86_64 `smp>1` 挂起调查，两者都需要
一块真正能出画面的显示器。

根因已定位（插桩实证）。把 `drm_gem_alloc` / `drm_free_gem` / `addfb` / `setcrtc`
四处放在一起打点，并关掉 IPv6 以排除 lwIP panic 干扰（这样桌面才活得久到能出
trace），x86_64 `NR_CPUS=4` 跑出来的序列是决定性的：

  ```
  [GP] alloc h1
  [GP] free h1
  [GP] setcrtc fb_id=1 MISS live=0
  [GP] setcrtc fb_id=2 MISS live=0
  ```

即帧缓冲的后备 GEM 在帧缓冲仍在使用时就已被释放。原因在实现里：
`drm_mode_addfb/addfb2` 直接把 `fb.fb_id = b->handle`，帧缓冲 ID 就是 GEM
handle 的别名，内核里没有独立的 framebuffer 对象，也没有任何引用计数。于是
wlroots 建完 `drmModeAddFB2` 后按 Linux 语义销毁 dumb buffer（`MODE_DESTROY_DUMB`
或 `GEM_CLOSE`），`drm_free_gem()` 就把那个 GEM 释放了；帧缓冲的 `fb_id` 随之悬空，
随后 `drmModeSetCrtc` 带着这个 `fb_id` 回来，`drm_find_gem()` 自然找不到 → `-ENOENT`。
这同时解释了「间歇性」：取决于 destroy 与 setcrtc 的先后。

正确修法已做：引入真正的 framebuffer 对象（`drm_fb_t`，持有对 GEM 的
引用），`ADDFB/ADDFB2` 从独立 id 空间分配 `fb_id`（`g_fbs[]`，
`DRM_MAX_FBS = 64`，`g_fb_next_id` 递增），`RMFB` 释放该引用。同批落地：

- CRTC 现在真的保存绑定（`g_crtc`，此前 `SETCRTC` 呈现成功并返回 0 却什么都不
  存，于是 `GETCRTC` 永远报 `fb_id 0`）；
- `PAGE_FLIP` 不再把 `pf.fb_id` 当 GEM handle 解析（framebuffer id 与 GEM handle
  来自两个独立计数器，此前只是碰巧相等才工作）；
- `GETFB2` 从后备 GEM 应答（此前是一个清零就返回成功的 stub）。

`mode_valid` 在 `fb_id == 0` 时故意保持 1；它表示"CRTC/connector 这一对
已编程了一个 mode"，不是"已绑定 framebuffer"，报 0 会让 wlroots 在 backend
init 阶段直接放弃这个 output。不能只在 `destroy_dumb` 上打补丁绕过，那只是把
悬空推迟到下一次。

复现要点：必须关掉 IPv6（`LWIP_IPV6=0` + `LWIP_ICMP6=0` 等）桌面才活得够久、
不被 lwIP panic 打断；且 trace 要在同一轮 boot 里同时打四处，跨轮次对比会自相矛盾。
