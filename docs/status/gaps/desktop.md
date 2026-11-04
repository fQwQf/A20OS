# DESKTOP 缺口

对应 V1-DESKTOP。现状见 [current/desktop-minecraft-status.md](../current/desktop-minecraft-status.md)；仍未关闭问题的诊断见 [notes/](notes/README.md)。电源菜单的关机、重启要求及验收结果见[桌面验收记录](../history/desktop-1.0-2026-10-23.md)。

### DESKTOP-14 `svcmgr` 强制终止在 AArch64 的验收

- 现状：RV64 dev 镜像上已验证健康超时 → `sig=9` 强停 → 恢复，含负控与 shutdown 审计。 AArch64 原生镜像上复跑同一路径时，`svcmgr` 能启动，但 `ufsd` 以`fs_serve failed st=-4`、`rtcd` 以 `claim failed` 反复退出，二者触发 flap budget 被标记 failed，`native-registry-aarch64` 在第一个 time RPC 就失败；桌面 world 本身不运行 svcmgr/echod/rtcd，也没有该测试二进制。
- 缺口：AArch64 上服务自身起不来，健康超时/强停/恢复链路因此无法验收；历史上该板的 ufsd 健康探测超时也未解释。
- 关闭条件：AArch64 桌面实例上完成 HANG 注入、强停、恢复与负控，审计干净；先解决 aarch64 上 ufsd/rtcd 的启动失败。
- 来源：原 `known-issues.md` 当前工程队列；AArch64 原生复跑验证记录。
