# 从源码适配新软件到 A20OS

目标平台是 VisionFive 2：一个没有现成 A20OS 端口的开源软件，要从源码编译成可运行的 RISC-V 程序，再放进 A20OS 的根文件系统或 extra 分区。宿主机二进制拷进去不算数：源码、交叉工具链、ABI、运行时依赖和许可证都要能被重复构建、检查和说明。

## 1. 先确定软件应该放在哪里

A20OS 的板上镜像有两个用户态文件系统：

| 位置 | 构建入口 | 适合的软件 |
|---|---|---|
| `/bin`（FAT32 rootfs） | `user/Makefile` | init、shell、基础命令、启动必需的小程序 |
| `/extra`（extra ext4） | world 清单 → `make image-world` | 绝大多数用户软件：直接用 Alpine 上游包 |

如果软件不是启动必需项，优先放 `/extra`。这样不会让 128 MiB 的 FAT32 根文件系统被开发工具挤满，而且要增减软件只需改 world 清单，不触碰构建系统。需要访问硬件的程序通常不是普通用户程序，应先阅读 [驱动开发手册](../drivers/README.md)，决定是内核驱动、`.a20drv` 模块还是 Native ABI 用户态服务。

**先问一句：Alpine 上游有没有这个包？** 大多数"要移植的软件"其实已经有
apk 包，写进 world 清单就能用。只有上游没有包、或包不满足需求时才需要自己
构建，而自己构建的路径现在只剩两条（见 [4. 接入方式](#4-选择接入方式)）：
新增一个 `user/cmds/` 程序，或 fastfetch 这类必须进 FAT32 根的组件。

## 2. 评估上游源码和目标约束

在写 Makefile 前记录以下事实：

1. 上游版本、commit、许可证、下载地址和所有递归 submodule。
2. 软件是 C、C++、Rust 还是需要生成代码的混合工程。
3. 是否支持 `riscv64`、`lp64d`、`rv64g`，是否假设 MMU、线程、`fork`、 `dlopen`、`/proc`、termcap、网络或图形设备存在。
4. 构建产物是静态 ELF、NOMMU `static-pie`，还是依赖动态加载器的 glibc ELF。执行 `file`、`readelf -h`、`readelf -d`，不要凭文件名猜测。
5. 运行时需要哪些配置文件、数据文件、插件、共享库和环境变量。

VisionFive 2 的 A20OS 构建默认使用 `ARCH=riscv64 NOMMU=1`，用户程序通常使用 musl 和静态链接；宿主机的 x86_64 可执行文件不能放入镜像。若软件必须使用 glibc 或 `dlopen`，必须把对应 RISC-V glibc 运行库和所有共享库一起放入 `/extra`，并在文档中明确这是动态程序——检查 ELF interpreter 和每一个 `DT_NEEDED` 库，不能只看主程序能否被 `file` 识别。走 apk 路线时这条约束由 Alpine 包自己满足：它们自带各自的 loader。

> 历史上这里举例的 Lamina1（一个依赖宿主 glibc 的 C++23 动态可执行文件）已随源码构建的 extra 路径一起删除。

## 3. 获取源码和固定版本

**走 apk 路线时这一节整节跳过**：版本由 world 清单 + Alpine 仓库的索引固定，
`make image-world` 每次都解析出同一个包，不下载、不打补丁、不 vendoring。

只有真的要自己构建时才需要关心源码。树里现存的 gitlink 只有 fastfetch 一个；
VisionFive 2 的源码准备脚本现在也只负责它（GitHub 默认走 SSH，没有密钥时切换
HTTPS）：

```sh
make vf2-extra-sources
VF2_GIT_TRANSPORT=https make vf2-extra-sources
```

若为自己的 `user/cmds/` 程序或新的 vendored 组件引入了 gitlink，使用：

```sh
git submodule sync --recursive
git submodule update --init --recursive -- user/external/apps/example
```

新软件若是正式依赖，应加入 `.gitmodules` 并固定 gitlink commit；不能在 Makefile 中无提示地下载 `main`。若上游没有稳定 git 仓库，可把压缩包校验和、版本号和解包步骤写入一个专用 `fetch-*.sh`，下载失败必须退出，禁止静默跳过软件。

获取后检查：

```sh
git submodule status --recursive
git -C user/external/apps/example rev-parse HEAD
```

## 4. 选择接入方式

### 4.1 基础命令：放入 `user/Makefile`

把单个 C 文件放入 `user/cmds/<group>/name.c`，当前 Makefile 会通过 `cmds/*/*.c` 自动发现本地命令。它会使用当前 `ARCH`、musl 头文件、CRT 和 `LIBC` 链接，并将二进制放进对应的 `user/build/<variant>/`。这种方式适合小型、启动后总是存在的命令；不要把大型第三方工程硬塞进通配符规则。

### 4.2 可选软件：作为 Alpine 上游包写进 world 清单

这是现在唯一的常规路径，也是 Git/Vim/GCC 的实际来源。绝大多数软件不需要写
任何 Makefile 代码，只需要在 world 清单里写一行包名：

```sh
$EDITOR packages/world/devel.world     # 或新建 packages/world/<名字>.world
```

`packages/world/*.world` 就是一份纯包名清单。已有的 `devel` world 是：

```text
a20-base
a20-drivers

# 来自 Alpine 官方仓库的上游包（musl 生态）
musl
busybox
vim
git
curl
ca-certificates
less
```

要加一个包，把包名追加进去即可；`a20-*` 是本仓库自己的 recipe 包，其余名字
由 `apk` 从 Alpine 官方仓库解析。改完直接组镜像验证：

```sh
make ARCH=riscv64 image-world PKG_WORLD=devel        # 组镜像
make ARCH=riscv64 run-world  PKG_WORLD=devel        # 组镜像并在 QEMU 跑
```

注意：

- 首次组镜像需要访问 Alpine 镜像源（`mkrootfs.py` 默认 USTC，可用
  `--alpine-mirror` 切换）；之后走 `build/cache/apk` 缓存。用
  `PKG_ALPINE=0` 可以做纯本地仓库组合，但那时清单里的上游包必须已经存在于
  本地仓库，否则解析失败。
- 镜像大小不够时用 `PKG_SIZE_MB` 调（默认 512；`xfce` 这类桌面 world 自动
  提到 4096）。这是**唯一**控制第二个 ext4 槽大小的旋钮。
- 包名写错时 `apk` 解析失败会让组镜像直接失败——这正是想要的：不能用"源目录
  不存在所以跳过"伪装成功。
- 许可证与分发义务由 Alpine 上游包自身的条款决定，不由本仓库决定；新增包
  后更新 [第三方声明](../THIRD_PARTY_NOTICES.md)（见第 8 节）。

旧的"加一个 `user/extra.mk` case、再写 recipe 打包"流程已经不存在，相关入口
（`extra-user-apps`、`extra-img`、`run-*-extra`）是会 `exit 2` 的弃置桩，会直接
告诉你用哪个 world 代替。

### 4.3 仍然从源码构建的两种情况

只有这两条路径还需要自己写构建代码：

1. **新增一个 `user/cmds/` 程序**（见 4.1）——本仓库自己的代码，走
   `user/Makefile`，产物进 FAT32 根。
2. **fastfetch**——它必须出现在任何 Alpine world 可用之前的 FAT32 根里，
   所以是唯一保留的 vendored 用户程序，也是 `.gitmodules` 里唯一的 gitlink。
   它由 `user/extra/fastfetch_gen_headers.sh` 生成所需头文件。

Alpine 上游已有等价包时不要走这两条路：自己构建一个上游已经维护好的程序，
只会引入上游安全更新跟不上的风险，而收益仅仅是"版本号是自己选的"。

对这两条路径里的复杂工程（CMake、Meson、Cargo 等），为每个工程建立隔离的
build 目录，明确传入交叉编译器和目标三元组。例如 CMake 至少应传入
`CMAKE_SYSTEM_NAME`、`CMAKE_SYSTEM_PROCESSOR=riscv64`、`CMAKE_C_COMPILER` 和
`CMAKE_FIND_ROOT_PATH`；Cargo 应固定 target、linker 和静态链接参数。不要让
CMake 在宿主机上运行目标程序探测能力；将结果通过 toolchain 文件、cache 变量或
`config.site` 提供。

如果软件需要宿主机生成器（protobuf、代码生成器等），把 host 工具和 target
工具拆成两个明确目标，不能用一个 `CC` 变量混用。

## 5. 工具链、并行和可重复性

常用构建与验证命令：

```sh
NPROC=$(getconf _NPROCESSORS_ONLN)

# 组一个 world 镜像并在 QEMU 里跑（第二块盘挂 /extra）
make -j"$NPROC" ARCH=riscv64 image-world PKG_WORLD=devel
make -j"$NPROC" ARCH=riscv64 run-world  PKG_WORLD=devel

# 只构建 A20OS 自己的用户态（user/cmds/、fastfetch）
make -j"$NPROC" -C user ARCH=riscv64 NOMMU=1 \
  BUILD_DIR=build/riscv64-nommu
```

QEMU 中的 `/extra` 挂载、程序输出和退出码都正常后，再生成 VisionFive 2 镜像。
QEMU 通过 VirtIO 块设备挂载 world 镜像，它不能替代真机对串口、JH7110 DTB、
SD 分区和板级驱动的验收。

板上编译不再是常规路径：`gcc` 作为 apk 包从 Alpine 上游取得（`devtools` world），
板上没有自举的交叉工具链。因此文档里曾描述的"先构建 musl-cross-make 再自举
GCC"的流程与 `MUSL_CROSS_ROOT` 已随源码树一起删除；宿主侧要编译 C/C++/Rust
目标程序时，用发行版自己的交叉工具链（见 [build.md](../build.md) 的环境准备），
产物再按第 6 节检查后放进 world 或 `user/cmds/`。

大型 C++ 工程会消耗大量内存，`-j$(nproc)` 只适合编译器支持并行的阶段；上游
单个翻译单元很大的工程应在配方中强制串行，避免并行 OOM。

每个构建目标都应把 ABI、编译器版本、优化选项和关键配置纳入 stamp。源码、
Makefile 或工具链改变后，stamp 必须失效；不要通过 `touch` 产物掩盖失败。

## 6. 安装到镜像和运行时检查

完整 VisionFive 2 镜像（`VF2_WORLD` 决定 `/extra` 分区里是哪套 apk 软件，
默认 `devel`）：

```sh
make -j"$NPROC" vf2-image VF2_WORLD=devel
# 换成开发工具链（gcc/musl-dev/fastfetch）：
make -j"$NPROC" vf2-image VF2_WORLD=devtools
# 桌面：
make -j"$NPROC" vf2-image VF2_WORLD=xfce
```

world 镜像按 world 清单里 `apk` 解析出的实际内容决定大小，不再有
`EXTRA_IMAGE_MB` 这类"必须大于 staging 内容"的估算；体积不够时调整
`PKG_SIZE_MB`。也可以用 `VF2_EXTRA_IMAGE=/path/to/prebuilt.img` 换成一个
预先构建好的分区镜像。

镜像生成后，先在主机只读挂载 extra 分区检查文件，不要等写卡后才发现漏了运行库。
apk 走 Alpine 的目录约定：普通程序在 `/usr/bin`，管理命令在 `/usr/sbin`、
`/sbin`，库在 `/usr/lib`——`/bin` 下只有 busybox 提供的 applet：

```sh
sudo mount -o ro,loop,offset=$((160*1024*1024)) \
  build/vf2-firmware/a20os-sd.img /mnt/a20os-extra
file /mnt/a20os-extra/usr/bin/example
readelf -h /mnt/a20os-extra/usr/bin/example
ldd /mnt/a20os-extra/usr/bin/example 2>/dev/null || true
sudo umount /mnt/a20os-extra
```

实际偏移应以 `sgdisk -p build/vf2-firmware/a20os-sd.img` 为准；不要固定假设 extra 永远从 160 MiB 开始，因为带 FAT32 rootfs 时脚本会按 rootfs 大小对齐。上板后验证：

```sh
mount
ls -l /extra/usr/bin /extra/usr/lib /extra/usr/share
/extra/usr/bin/example --version
```

程序若通过 PATH 调用，确认 shell 的 PATH 包含 `/extra/usr/bin`；否则使用绝对路径。带 `a20-distro` 标记的 world（如 `devtools`、`xfce`）会在 init 阶段 chroot 进 `/extra`，此时直接用 `example --version` 即可，不必写 `/extra/` 前缀。配置文件、Vim runtime、Git templates 等非二进制资源由 apk 包一并安装到包自己声明的路径；如果你为 `user/cmds/` 程序或 fastfetch 打了补丁，则要自己确认这些资源落在脚本实际查找的位置。动态程序还要检查 ELF interpreter 和每一个 `DT_NEEDED` 库，不能只看主程序能否被 `file` 识别。

## 7. 测试清单

提交前至少完成以下测试：

```sh
git diff --check

# 改的是 world 清单：重新组镜像并在 QEMU 验证
make -j"$NPROC" ARCH=riscv64 image-world PKG_WORLD=devel
make -j"$NPROC" ARCH=riscv64 run-world  PKG_WORLD=devel

# 改的是 user/cmds/ 或 fastfetch：重建用户态，再出卡
make -j"$NPROC" -C user ARCH=riscv64 NOMMU=1 \
  BUILD_DIR=build/riscv64-nommu
make -j"$NPROC" vf2-image VF2_WORLD=devel
```

在 QEMU 或板上分别测试：正常启动、`--help`/`--version`、读写文件、错误参数、标准输入输出、信号退出和重复执行。程序如果使用网络、时间、线程、终端或 `/proc`，增加对应的最小回归用例；不要只验证能启动一次。

失败时保留 `apk` 解析输出、`readelf -a` 摘要、镜像分区表和串口日志。先用最小
world（`min`）判断是内核/启动链问题还是新软件问题，再逐个把包加回去。

## 8. 许可证和提交要求

走 apk 路线时，许可证与分发义务由 Alpine 上游包自身的条款决定，不需要在
[第三方声明](../THIRD_PARTY_NOTICES.md) 里逐个登记。只有 4.3 那两条仍从源码
构建的路径需要更新它：记录精确版本、许可证、是否静态链接、是否带共享库、
源码获取方式和镜像内路径。提交前确认：

- vendor submodule 没有被构建过程弄脏；
- 生成物不被错误地加入 Git；
- `git diff --check`、对应架构编译和最小运行测试通过；
- 文档写明软件的架构限制、运行时依赖和已知缺陷；
- 不能用“源目录不存在所以跳过”伪装成功，缺少必需源码应让构建失败。

这样接入的新软件既能在 QEMU 中快速迭代，也能随 [VisionFive 2 上板启动流程](../platforms/visionfive2-boot.md) 生成可审计、可复现的 TF 卡镜像。
