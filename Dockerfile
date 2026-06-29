FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    gcc-riscv64-unknown-elf \
    gcc-loongarch64-linux-gnu \
    gcc-aarch64-linux-gnu \
    gcc-x86-64-linux-gnu \
    qemu-system-misc \
    qemu-system-x86 \
    qemu-system-arm \
    python3 \
    git \
    vim \
    rustc \
    cargo \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /oskernel
COPY . /oskernel/

CMD ["make", "all"]
