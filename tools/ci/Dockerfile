# A20OS 构建环境 —— CI 与本地共用同一个容器，保证“在我机器上能编”成立。
#
# 本地使用：
#   docker build -t a20os-buildenv tools/ci
#   docker run --rm -v "$PWD:/src" -w /src a20os-buildenv \
#       make ARCH=riscv64 dev-build
#
# 对应文档：docs/packaging/ci.md
FROM debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive

# 与 docs/build.md 的“环境准备”一节保持一致；新增依赖时两边同步。
# 注意：ccache 必装（CI 缓存依赖它），fakeroot 供 mkrootfs --usermode 使用。
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential curl git ca-certificates xz-utils file \
        dosfstools e2fsprogs mtools fakeroot sudo openssl \
        qemu-system-misc qemu-system-x86 qemu-system-arm \
        gcc-riscv64-unknown-elf gcc-riscv64-linux-gnu g++-riscv64-linux-gnu \
        gcc-aarch64-linux-gnu gcc-x86-64-linux-gnu \
        gcc-powerpc64le-linux-gnu gcc-arm-linux-gnueabihf \
        gcc-arm-none-eabi binutils-arm-none-eabi \
        python3 ccache \
    && rm -rf /var/lib/apt/lists/*

# LoongArch64 交叉工具链在 Debian 主源的 cross-ports 里；个别版本缺失时
# 按 docs/build.md 的说明改用 Loongson 官方工具链。
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-loongarch64-linux-gnu \
    && rm -rf /var/lib/apt/lists/* \
    || echo "warning: gcc-loongarch64-linux-gnu unavailable in this snapshot"

WORKDIR /src
CMD ["bash"]
