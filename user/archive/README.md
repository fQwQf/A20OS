# user/archive/ 历史参考代码

这个目录保存 A20OS Native ABI 早期用户态代码的历史参考实现，不参与当前构建。

## 内容说明

- `a20coreutils/`：旧版原生 coreutils 示例（cat、cp、echo、env、mkdir、pwd、rm、touch 等）。
- `a20libc/`：更早期的最小 libc 尝试。
- `a20sh.c`：旧版原生 shell 示例。
- `arch/a20/`：旧版 musl `arch/a20/` 适配头（syscall.h、crt_arch.h、atomic.h、reloc.h、pthread_arch.h、bits/syscall.h）。
- `src/`：旧版 musl 桥接代码，包括 pthread、mutex、signal、fork/posix_spawn、fdtable、syscallops 等。
- `build_sysroot.sh`：旧版 sysroot 构建脚本，引用了 `user/musl-port/` 等当前仓库中不存在的路径。
- `tests/`：旧版测试程序。
- `config.a20`、`a20.ld`、`stress.c`、`syscall_bridge.c`、`contest_init/`：早期实验代码和比赛初始化材料。

## 重要提示

- 本目录**不参与当前构建**。Makefile、CMake 或任何构建脚本都不会自动编译这里的文件。
- 部分文件引用了已经不存在的路径（例如 `user/musl-port/`）。直接运行 `build_sysroot.sh` 会失败。
- 当前活跃的原生 SDK 在 `user/liba20rt/`，最小原生 C 库在 `user/liba20c/`。
- 当前主用户态运行时是 Linux ABI 之上的 musl 兼容层，由 `kernel/abi/linux/` 提供接口。
- 如果未来需要重新启动完整的 Native POSIX 兼容层或 musl 移植，可以本目录作为参考，但需要重新设计路径和实现，而不是直接复用。
