# arm32 的 FP 上下文尚未修复

> 来源：原 `docs/status/current/known-issues.md` 的对应小节；该文件已拆分删除。
> 关联台账：[CORE-07](../core.md)。

riscv32、ppc64le 已补上 trap 帧的 FP 保存（ppc64le 仅编译验证）；arm32 仍未修，本机无 arm 工具链，
且其 trap 帧与 trap.S 用逐字段断言强绑定，改动未经验证。

判定标准是「用户态是否启用 FP/SIMD」：arm32（`-mfpu=vfpv3-d16 -mfloat-abi=hard`）、riscv32（`ilp32d`）、
ppc64le（`MSR_FP`）都启用；loongarch32/armv7m 无 FPU，不受影响。ppc64le 的向量保存受 `MSR_VEC`
门控而该位未设，需另存 FPR。
