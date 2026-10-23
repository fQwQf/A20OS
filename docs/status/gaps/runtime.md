# RUNTIME 缺口

对应 V1-RUNTIME。动态链接、exec 继承、kexec 边界与 NOMMU 平台矩阵的现状见
[current/native-runtime-1.0.md](../current/native-runtime-1.0.md)。

### RUNTIME-03 Java 工作负载

- 现状：JVM 能启动，部分应用可运行；退出异常与 GC/JIT 行为有历史问题。
- 缺口：没有端到端的 Java/JIT/GC 工作负载与清理契约。
- 关闭条件：固定 Java 程序完成启动、工作、退出与资源回收，冷启动与峰值内存可复现。
- 阻塞：`tools/build-jamvm-*.py` 需要冻结的 jamvm/zlib/libffi/classpath 归档与
  `ecj-4.6.1.jar`。四个源码归档已按脚本内 SHA-256 校验通过，但 `ecj-4.6.1.jar`
  在各公开镜像上均已 404，仓库内也没有该 jar；Java/GC 验收因此无法在本分支重跑。
  该工作流与并在的 Minecraft/Java 工作树重叠，由对应 owner 提供固定输入并执行。
- 来源：[current/v1.0-progress.md](../current/v1.0-progress.md) V1-RUNTIME 行。
