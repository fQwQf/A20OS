# A20OS 构建镜像：一个共享 base，每个架构一个 target。
#
# 为什么按架构拆：单镜像虽然"什么都能编"，但它要求每个 job 拉下所有架构的
# 工具链；更糟的是缺包是隐形的——只有真的去编那个架构才会暴露。这个镜像正是
# 这样缺过两样东西：每个容器内内核构建都要的 Rust 一点没有，armhf 的 libc
# 开发头也没有（交叉 gcc 只把 libc6-dev-*-cross 列为 Recommends，而本文件用
# --no-install-recommends，于是装了个头文件不全的编译器）。按架构拆 + 每个
# target 自带自检，才能把这类缺失变成"拉这个 target 就报错"。
#
# 本地使用（Docker 只是辅助；用宿主自己的工具链不需要它）：
#   docker build --target multi -t a20os-buildenv tools/ci
#   docker run --rm -v "$PWD:/src" -w /src a20os-buildenv \
#       make ARCH=riscv64 dev-build
#   tools/ci/build-image.sh riscv          # 构建 + 自检某一个 target
#
# CI：.github/workflows/buildenv.yml 构建并推送全部 target。
# 文档：docs/packaging/ci.md、docs/dev/build.md
#
# 各架构的包清单只写在下面的 ARG 里，`multi` 直接组合它们，因此单架构镜像与
# 全能镜像不会各记一份而漂移。

ARG RUST_VERSION=1.96.0

FROM debian:trixie-slim AS base
ARG RUST_VERSION

ENV DEBIAN_FRONTEND=noninteractive
ENV RUSTUP_HOME=/opt/rustup \
    CARGO_HOME=/opt/cargo
ENV PATH=/opt/cargo/bin:$PATH

# 与架构无关的部分：镜像制作工具、x86_64 宿主编译器、ccache（CI 缓存依赖它）、
# fakeroot（供 mkrootfs --usermode）、qemu-user-static（供 guest 内 chroot 的
# Alpine 世界与 binfmt 注册）。
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential curl git ca-certificates xz-utils file \
        dosfstools e2fsprogs mtools fakeroot sudo openssl \
        qemu-user-static \
        python3 ccache ripgrep \
    && rm -rf /var/lib/apt/lists/*

# Rust 放在 base：每个架构的内核与用户态构建都要它。rust-src 不是可选项——
# riscv32 是自定义 JSON 目标，用 `-Z build-std=core` 现场编译 core，而 core
# 来自 rust-src 组件。rustc 版本与 delivery 基线的 1.96.0 对齐；宿主 std 保留，
# 因为 check-a20-tests 的宿主侧 Rust 用例（如 shutdown_write 的线程测试）用
# `rustc --test` 在本机目标上编译。
#
# 每个架构的 no_std target 反而**不**装在这里：一个 target 的解压体积是
# 60–80 MB，七个加起来比任何单个交叉 gcc 都大。它们和交叉 gcc 一样，属于
# "这个架构才需要"的东西，装在各架构的 stage 里。
RUN curl -sSf https://sh.rustup.rs \
      | sh -s -- -y --profile minimal --default-toolchain "$RUST_VERSION" \
    && rustup component add rust-src --toolchain "$RUST_VERSION"

ARG RISCV_PKGS="gcc-riscv64-unknown-elf gcc-riscv64-linux-gnu g++-riscv64-linux-gnu libc6-dev-riscv64-cross qemu-system-riscv"
ARG RISCV_RUST_TARGETS="riscv64imac-unknown-none-elf riscv64gc-unknown-none-elf"
ARG AARCH64_PKGS="gcc-aarch64-linux-gnu libc6-dev-arm64-cross qemu-system-arm"
ARG AARCH64_RUST_TARGETS="aarch64-unknown-none"
# x86_64 的内核与用户态都用 base 里的宿主 gcc，只需要模拟器。
ARG X86_64_PKGS="qemu-system-x86"
ARG X86_64_RUST_TARGETS="x86_64-unknown-none"
ARG LOONGARCH64_PKGS="gcc-loongarch64-linux-gnu libc6-dev-loong64-cross qemu-system-misc"
ARG LOONGARCH64_RUST_TARGETS="loongarch64-unknown-none"
ARG ARM32_PKGS="gcc-arm-linux-gnueabihf libc6-dev-armhf-cross qemu-system-arm"
ARG ARM32_RUST_TARGETS="armv7a-none-eabihf"
ARG ARMV7M_PKGS="gcc-arm-none-eabi binutils-arm-none-eabi qemu-system-arm"
ARG ARMV7M_RUST_TARGETS="thumbv7m-none-eabi"

# riscv64 与 riscv32 共用一个镜像：内核的 rv32 构建就是同一个
# riscv64-unknown-elf 工具链的 rv32 multilib，拆开只会把它装两遍；rv32 的
# Rust core 由 -Z build-std 现场编译，所以没有对应的 rustup target。

FROM base AS riscv
ARG RUST_VERSION
ARG RISCV_PKGS RISCV_RUST_TARGETS
RUN apt-get update && apt-get install -y --no-install-recommends $RISCV_PKGS \
    && rm -rf /var/lib/apt/lists/* \
    && rustup target add --toolchain "$RUST_VERSION" $RISCV_RUST_TARGETS

FROM base AS aarch64
ARG RUST_VERSION
ARG AARCH64_PKGS AARCH64_RUST_TARGETS
RUN apt-get update && apt-get install -y --no-install-recommends $AARCH64_PKGS \
    && rm -rf /var/lib/apt/lists/* \
    && rustup target add --toolchain "$RUST_VERSION" $AARCH64_RUST_TARGETS

FROM base AS x86_64
ARG RUST_VERSION
ARG X86_64_PKGS X86_64_RUST_TARGETS
RUN apt-get update && apt-get install -y --no-install-recommends $X86_64_PKGS \
    && rm -rf /var/lib/apt/lists/* \
    && rustup target add --toolchain "$RUST_VERSION" $X86_64_RUST_TARGETS

# loongarch64 的交叉工具链只在 Debian cross-ports 里。这里不吞掉安装失败：
# 一个"少一个架构"的镜像正是本轮要消除的那类静默缺失，装不上就让这个 target
# 构建失败，由 docs/dev/build.md 记录的备选工具链路径接手。
FROM base AS loongarch64
ARG RUST_VERSION
ARG LOONGARCH64_PKGS LOONGARCH64_RUST_TARGETS
RUN apt-get update && apt-get install -y --no-install-recommends $LOONGARCH64_PKGS \
    && rm -rf /var/lib/apt/lists/* \
    && rustup target add --toolchain "$RUST_VERSION" $LOONGARCH64_RUST_TARGETS

FROM base AS arm32
ARG RUST_VERSION
ARG ARM32_PKGS ARM32_RUST_TARGETS
RUN apt-get update && apt-get install -y --no-install-recommends $ARM32_PKGS \
    && rm -rf /var/lib/apt/lists/* \
    && rustup target add --toolchain "$RUST_VERSION" $ARM32_RUST_TARGETS

FROM base AS armv7m
ARG RUST_VERSION
ARG ARMV7M_PKGS ARMV7M_RUST_TARGETS
RUN apt-get update && apt-get install -y --no-install-recommends $ARMV7M_PKGS \
    && rm -rf /var/lib/apt/lists/* \
    && rustup target add --toolchain "$RUST_VERSION" $ARMV7M_RUST_TARGETS

# 全集：给一次 make 就遍历多架构的门禁（check-kernel-build-all、check-user-build-all）
# 和需要三种架构同时存在的门禁（tools/check-cluster-instance.py）用。
# 这个 target 保持发布为 `:latest`，所以现有引用不需要改。
FROM base AS multi
ARG RUST_VERSION
ARG RISCV_PKGS AARCH64_PKGS X86_64_PKGS LOONGARCH64_PKGS ARM32_PKGS ARMV7M_PKGS
ARG RISCV_RUST_TARGETS AARCH64_RUST_TARGETS X86_64_RUST_TARGETS \
    LOONGARCH64_RUST_TARGETS ARM32_RUST_TARGETS ARMV7M_RUST_TARGETS
RUN apt-get update && apt-get install -y --no-install-recommends \
        $RISCV_PKGS $AARCH64_PKGS $X86_64_PKGS $LOONGARCH64_PKGS \
        $ARM32_PKGS $ARMV7M_PKGS \
    && rm -rf /var/lib/apt/lists/* \
    && rustup target add --toolchain "$RUST_VERSION" \
        $RISCV_RUST_TARGETS $AARCH64_RUST_TARGETS $X86_64_RUST_TARGETS \
        $LOONGARCH64_RUST_TARGETS $ARM32_RUST_TARGETS $ARMV7M_RUST_TARGETS

WORKDIR /src
CMD ["bash"]
