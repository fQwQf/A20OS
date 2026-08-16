# A20OS cross-compilation build environment.
#
# The base image is pinned by digest so the OS layer is reproducible; apt
# package versions resolve against that snapshot at build time.  The judged
# flow runs `conda run -n a20os python` (Makefile), so the image provisions a
# miniconda `a20os` environment instead of silently falling back to python3.
# Keep the pinned digest/installer/versions in sync with the judged host.
FROM ubuntu@sha256:561618e2c15bf2397621dd04f96926663a3b5616c189cf7e38db7e82f5c538ea

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    binutils-arm-none-eabi \
    build-essential \
    curl \
    dosfstools \
    e2fsprogs \
    file \
    gcc-arm-linux-gnueabihf \
    gcc-arm-none-eabi \
    gcc-riscv64-unknown-elf \
    gcc-riscv64-linux-gnu \
    g++-riscv64-linux-gnu \
    gcc-aarch64-linux-gnu \
    gcc-x86-64-linux-gnu \
    gcc-powerpc64le-linux-gnu \
    mtools \
    openocd \
    qemu-system-misc \
    qemu-system-x86 \
    qemu-system-arm \
    python3 \
    python3-pip \
    ripgrep \
    git \
    vim \
    wget \
    xz-utils \
    rustc \
    cargo \
    && rm -rf /var/lib/apt/lists/*

# LoongArch64: Ubuntu 24.04 has no gcc-loongarch64-linux-gnu in its archive;
# install the official Loongson cross toolchain separately (see
# docs/build.md) before building ARCH=loongarch64.

# lamina (extra pkg) needs CMake >= 3.29; Ubuntu 24.04's apt cmake is 3.28.
# Pin the pip version so builds are reproducible across image rebuilds.
RUN python3 -m pip install --no-cache-dir --break-system-packages cmake==3.29.6

# Match the judged toolchain flow: the Makefile invokes
# `conda run -n a20os python` when conda is present.  The env carries only
# python here; judge-specific packages are installed by the runner.
RUN curl -fsSL -o /tmp/miniconda.sh \
        https://repo.anaconda.com/miniconda/Miniconda3-py311_26.5.3-2-Linux-x86_64.sh \
    && bash /tmp/miniconda.sh -b -p /opt/miniconda3 \
    && rm /tmp/miniconda.sh \
    && /opt/miniconda3/bin/conda create -y -n a20os python=3.11 \
    && /opt/miniconda3/bin/conda clean -ay
ENV PATH="/opt/miniconda3/bin:${PATH}"

WORKDIR /oskernel
COPY . /oskernel/

CMD ["make", "all"]
