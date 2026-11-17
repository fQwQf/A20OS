# DELIVERY 缺口

对应 V1-DELIVERY。现状见 [current/delivery-baseline.md](../current/delivery-baseline.md)。

### DELIVERY-03 当前候选尚未完成云端 CI

- 现状：用户已授权推送和派发。第六轮的宿主整组、六架构编译与两行 Native 已通过；x86 门禁因四核 runner 不能提供四个完全空闲 CPU 而失败，剩余流程已停止。常规配置改为 SMP2，发布产物验收仍待实际结果，见[候选记录](../current/v1.0-candidate.md)。
- 缺口：需完成新分层工作流的候选验收并记录全部 job/skip。首轮已定位子模块、BSD 前端和宿主依赖问题，修复不等于云端已通过；旧镜像发布记录也不能认证新候选。
- 本地现状：N18/N20 已关闭。现按项目负责人要求区分常规 CI、扩展验收和产物发布，具体范围见[CI/CD 说明](../../packaging/ci.md)；扩展层未运行必须披露，不计作通过。
- 关闭条件：候选提交上的工作流完整跑完；每个 skipped job 都有分类，且 skipped 不计入覆盖。需保存远端候选提交、工作流和各 job 的真实结果。
- 来源：requirements N02。
