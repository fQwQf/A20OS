FROM ubuntu:24.04

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
    gcc-loongarch64-linux-gnu \
    gcc-aarch64-linux-gnu \
    gcc-x86-64-linux-gnu \
    gcc-powerpc64le-linux-gnu \
    mtools \
    openocd \
    qemu-system-misc \
    qemu-system-x86 \
    qemu-system-arm \
    python3 \
    ripgrep \
    git \
    vim \
    wget \
    xz-utils \
    rustc \
    cargo \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /oskernel
COPY . /oskernel/

CMD ["make", "all"]
