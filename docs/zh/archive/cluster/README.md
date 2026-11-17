# 集群子系统历史研发档案 (Cluster Historical Archive)

本目录收录 A20OS 集群子系统在早期开发、逐步迭代与功能原型验证阶段产生的历史实验笔记、单次测试记录与阶段性设计草稿（原 `docs/zh/cluster/` 下的编号文档）。

---

## 现役核心规范索引

集群子系统的正式现役规范已收敛至 `docs/zh/cluster/`：
* [docs/zh/cluster/overview.md](../../cluster/overview.md)：集群架构总览、分层模型与节点分级
* [docs/zh/cluster/02-wire-protocol.md](../../cluster/02-wire-protocol.md)：线缆帧格式与传输层契约
* [docs/zh/cluster/runtime-ssi.md](../../cluster/runtime-ssi.md)：单系统映像（SSI）与执行域抽象
* [docs/zh/cluster/mcu-shared-kernel.md](../../cluster/mcu-shared-kernel.md)：多 MCU 共享内存内核对称多处理 (SMP)
* [docs/zh/cluster/cluster-apps.md](../../cluster/cluster-apps.md)：应用开发模型与作业调度

---

## 历史档案清单

本目录保留历史记录供开发追溯与问题回溯，内容已冻结，不再随主干 HEAD 同步更新。
