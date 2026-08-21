# 从源码适配新软件到 A20OS

本文说明如何把一个没有现成 A20OS 端口的开源软件，从源码编译成
VisionFive 2 可运行的 RISC-V 程序，并放入 A20OS 的根文件系统或 extra
分区。目标不是把宿主机二进制拷进去，而是让源码、交叉工具链、ABI、运行时
依赖和许可证都能被重复构建、检查和说明。

## 1. 先确定软件应该放在哪里

A20OS 的板上镜像有两个用户态文件系统：

| 位置 | 构建入口 | 适合的软件 |
|---|---|---|
| `/bin`（FAT32 rootfs） | `user/Makefile` | init、shell、基础命令、启动必需的小程序 |
| `/extra`（extra ext4） | `user/extra.mk` + `tools/targets-extra.mk` | Git、Vim、GCC、Rust、实验工具和体积较大的可选软件 |

如果软件不是启动必需项，优先放 `/extra`。这样不会让 128 MiB 的 FAT32
根文件系统被开发工具挤满，也可以用 `EXTRA_PACKAGES` 独立增减。需要访问
硬件的程序通常不是普通用户程序，应先阅读
[驱动开发手册](../drivers/README.md)，决定是内核驱动、`.a20drv` 模块还是
Native ABI 用户态服务。

## 2. 评估上游源码和目标约束

在写 Makefile 前记录以下事实：

1. 上游版本、commit、许可证、下载地址和所有递归 submodule。
2. 软件是 C、C++、Rust 还是需要生成代码的混合工程。
3. 是否支持 `riscv64`、`lp64d`、`rv64g`，是否假设 MMU、线程、`fork`、
   `dlopen`、`/proc`、termcap、网络或图形设备存在。
4. 构建产物是静态 ELF、NOMMU `static-pie`，还是依赖动态加载器的 glibc
   ELF。执行 `file`、`readelf -h`、`readelf -d`，不要凭文件名猜测。
5. 运行时需要哪些配置文件、数据文件、插件、共享库和环境变量。

VisionFive 2 的 A20OS 构建默认使用 `ARCH=riscv64 NOMMU=1`，用户程序通常
使用 musl 和静态链接；宿主机的 x86_64 可执行文件不能放入镜像。若软件
必须使用 glibc 或 `dlopen`，必须把对应 RISC-V glibc 运行库和所有共享库
一起放入 `/extra`，并在文档中明确这是动态程序。Lamina 的做法就是把主程序、
四个共享库和 glibc 运行时一起 staging，而不是假装它是静态程序。

## 3. 获取源码和固定版本

已有的 gitlink 使用：

```sh
git submodule sync --recursive
git submodule update --init --recursive -- user/external/apps/example
```

VisionFive 2 extra 源码统一由脚本准备；GitHub 默认走 SSH，没有密钥时切换
HTTPS：

```sh
make vf2-extra-sources
VF2_GIT_TRANSPORT=https make vf2-extra-sources
```

新软件若是正式依赖，应加入 `.gitmodules` 并固定 gitlink commit；不能在
Makefile 中无提示地下载 `main`。若上游没有稳定 git 仓库，可把压缩包校验和、
版本号和解包步骤写入一个专用 `fetch-*.sh`，下载失败必须退出，禁止静默
跳过软件。

获取后检查：

```sh
git submodule status --recursive
git -C user/external/apps/example rev-parse HEAD
```

## 4. 选择接入方式

### 4.1 基础命令：放入 `user/Makefile`

把单个 C 文件放入 `user/cmds/<group>/name.c`，当前 Makefile 会通过
`cmds/*/*.c` 自动发现本地命令。它会使用当前 `ARCH`、musl 头文件、CRT 和
`LIBC` 链接，并将二进制放进对应的 `user/build/<variant>/`。这种方式适合
小型、启动后总是存在的命令；不要把大型第三方工程硬塞进通配符规则。

### 4.2 可选软件：在 `user/extra.mk` 增加包

这是 Git/Vim/GCC 的模式。一个新包至少要有：

```make
APP_SRC   := $(USER_DIR)/external/apps/example
APP_BUILD := $(OBJ_DIR)/example
APP_BIN   := $(BUILD_DIR)/example
APP_STAMP := $(STAMP_DIR)/.example-built
APP_AVAILABLE := $(if $(wildcard $(APP_SRC)/configure),1)

$(APP_BIN): $(APP_STAMP)
	@mkdir -p $(BUILD_DIR)
	cp $(APP_BUILD)/src/example $@
	$(CROSS_COMPILE)strip $@ 2>/dev/null || true

$(APP_STAMP): $(MUSL_CHECK_FILES) $(EXTRA_MAKEFILE) | musl_check
ifeq ($(APP_AVAILABLE),)
	@echo "[EXTRA] example source unavailable; skipping"
	@mkdir -p $(STAMP_DIR) && touch $@
else
	@rm -rf $(APP_BUILD)/src && mkdir -p $(APP_BUILD)/src $(STAMP_DIR)
	@cp -a $(APP_SRC)/. $(APP_BUILD)/src/
	cd $(APP_BUILD)/src && ./configure --host=$(CROSS_COMPILE:%-=%) \
		CC="$(CC)" CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)"
	$(MAKE) -C $(APP_BUILD)/src CC="$(CC)" \
		CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" \
		LIBS="$(CRT_START) $(LIBC) $(CRT_END)"
	@touch $@
endif
```

示例只展示结构，不是可以无修改复制的通用配方：每个上游项目的
`configure` 变量、链接顺序和生成文件都不同。把源码复制到 `obj` 工作树，
不要在 vendor submodule 中直接运行会改写源码的 configure 或 make；这样可
保持 submodule 干净，也避免并行构建互相覆盖。

然后把包接入四个位置：

1. `VALID_PACKAGES`：登记用户可输入的名称。
2. `REQUESTED_TARGETS` 和 `all`：让 `PACKAGES=example` 真正触发构建。
3. `tools/targets-extra.mk` 的 staging 循环：把生成的二进制、配置、数据和
   共享库复制到 `$(EXTRA_STAGING_DIR)`。
4. `EXTRA_IMAGE_STAMP` 的输入清单：记录源码/产物变化，使源码修改会使
   `extra.img` 重新生成，而不是误用旧镜像。

若命令名称与包名不同（例如 `gcc` 还要安装 `cc`、libexec、include），必须
显式列出每个文件，不能只复制一个顶层二进制。

### 4.3 需要生成代码或复杂构建系统的软件

在 `user/extra.mk` 中为 CMake、Meson、Cargo 等工程建立隔离的 build 目录，
明确传入交叉编译器和目标三元组。例如 CMake 至少应传入
`CMAKE_SYSTEM_NAME`、`CMAKE_SYSTEM_PROCESSOR=riscv64`、`CMAKE_C_COMPILER`
和 `CMAKE_FIND_ROOT_PATH`；Cargo 应固定 target、linker 和静态链接参数。
不要让 CMake 在宿主机上运行目标程序探测能力；将结果通过 toolchain 文件、
cache 变量或 `config.site` 提供。

如果软件需要宿主机生成器（protobuf、代码生成器等），把 host 工具和 target
工具拆成两个明确目标，不能用一个 `CC` 变量混用。

## 5. 工具链、并行和可重复性

常用板上构建命令：

```sh
NPROC=$(getconf _NPROCESSORS_ONLN)
make -j"$NPROC" -C user ARCH=riscv64 NOMMU=1 \
  BUILD_DIR=build/riscv64-nommu
make -j"$NPROC" -f user/extra.mk ARCH=riscv64 \
  PACKAGES=example
```

上面的 `example` 表示已经登记到 `user/extra.mk` 的包名；在包接入完成前，
不要把它当作可直接运行的现成目标。完整包还可以先在 QEMU 中验证：

```sh
make -j"$NPROC" run-riscv64-extra \
  EXTRA_PACKAGES='example vim git'
```

QEMU 中的 `/extra` 挂载、程序输出和退出码都正常后，再生成 VisionFive 2
镜像。QEMU 通过 VirtIO 块设备挂载 `extra.img`，它不能替代真机对串口、
JH7110 DTB、SD 分区和板级驱动的验收。

GCC 的 native RISC-V 过程会先复用/构建 `musl-cross-make` 工具链，再生成板上
运行的 GCC；`MUSL_CROSS_ROOT` 可指向已有的
`user/external/toolchain/musl-cross-make/output`。Rust、GCC 和大型 C++ 工程
会消耗大量内存，`-j$(nproc)` 只适合编译器支持并行的阶段；像 Lamina 这样
上游单个 C++ 翻译单元很大的工程应在配方中强制串行，避免并行 OOM。

每个构建目标都应把 ABI、编译器版本、优化选项和关键配置纳入 stamp。源码、
Makefile 或工具链改变后，stamp 必须失效；不要通过 `touch` 产物掩盖失败。

## 6. 安装到镜像和运行时检查

完整 VisionFive 2 镜像：

```sh
make -j"$NPROC" vf2-image \
  EXTRA_PACKAGES='example vim git' EXTRA_IMAGE_MB=1024
```

镜像生成后，先在主机只读挂载 extra 分区检查文件，不要等写卡后才发现漏了
运行库：

```sh
sudo mount -o ro,loop,offset=$((160*1024*1024)) \
  build/vf2-firmware/a20os-sd.img /mnt/a20os-extra
file /mnt/a20os-extra/bin/example
readelf -h /mnt/a20os-extra/bin/example
ldd /mnt/a20os-extra/bin/example 2>/dev/null || true
sudo umount /mnt/a20os-extra
```

实际偏移应以 `sgdisk -p build/vf2-firmware/a20os-sd.img` 为准；不要固定假设
extra 永远从 160 MiB 开始，因为带 FAT32 rootfs 时脚本会按 rootfs 大小对齐。
上板后验证：

```sh
mount
ls -l /extra/bin /extra/lib /extra/share
/extra/bin/example --version
```

程序若通过 PATH 调用，确认 shell 的 PATH 包含 `/extra/bin`；否则使用绝对
路径。配置文件、Vim runtime、Git templates 等非二进制资源必须和程序一起
安装到脚本实际使用的路径。动态程序还要检查 ELF interpreter 和每一个
`DT_NEEDED` 库，不能只看主程序能否被 `file` 识别。

## 7. 测试清单

提交前至少完成以下测试：

```sh
git diff --check
make -f user/extra.mk ARCH=riscv64 PACKAGES=example clean
make -j"$NPROC" -f user/extra.mk ARCH=riscv64 PACKAGES=example
make -j"$NPROC" vf2-image EXTRA_PACKAGES=example EXTRA_IMAGE_MB=512
```

在 QEMU 或板上分别测试：正常启动、`--help`/`--version`、读写文件、错误
参数、标准输入输出、信号退出和重复执行。程序如果使用网络、时间、线程、
终端或 `/proc`，增加对应的最小回归用例；不要只验证能启动一次。

失败时保留 configure 日志、交叉编译命令、`readelf -a` 摘要、镜像分区表和
串口日志。先用 `EXTRA_PACKAGES=''` 的最小镜像判断是内核/启动链问题还是
新软件问题，再逐个加入包。

## 8. 许可证和提交要求

新软件接入必须更新 [第三方声明](../THIRD_PARTY_NOTICES.md)：记录精确版本、
许可证、是否静态链接、是否带共享库、源码获取方式和镜像内路径。提交前
确认：

- vendor submodule 没有被构建过程弄脏；
- 生成物不被错误地加入 Git；
- `git diff --check`、对应架构编译和最小运行测试通过；
- 文档写明软件的架构限制、运行时依赖和已知缺陷；
- 不能用“源目录不存在所以跳过”伪装成功，缺少必需源码应让构建失败。

这样接入的新软件既能在 QEMU 中快速迭代，也能随
[VisionFive 2 上板启动流程](../platforms/visionfive2-boot.md) 生成可审计、可
复现的 TF 卡镜像。
