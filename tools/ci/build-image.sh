#!/bin/sh
# 构建并自检一个（或多个）buildenv target。
#
#   tools/ci/build-image.sh riscv
#   tools/ci/build-image.sh riscv aarch64 multi
#   tools/ci/build-image.sh all
#
# 每个 target 构建完立刻在容器里跑 tools/ci/image-selftest.sh，所以"这个镜像
# 齐全"不是假设而是这次运行的结果。镜像名是 a20os-buildenv:<target>；
# `multi` 额外打上 :latest，因为现有 CI job 与文档都按 :latest 拉它。
#
# 只依赖本机 docker；不需要 GHCR 凭据。推送由
# .github/workflows/buildenv.yml 负责。
set -eu

targets_all="base riscv aarch64 x86_64 loongarch64 arm32 armv7m multi"
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if [ "$#" -eq 0 ]; then
    echo "usage: build-image.sh <target>... | all" >&2
    echo "targets: $targets_all" >&2
    exit 64
fi

if [ "$1" = all ]; then
    # 单架构 target 在前、multi 在后：multi 的层能从前面刚构建的镜像里复用。
    set -- $targets_all
fi

for target in "$@"; do
    case " $targets_all " in
    *" $target "*) ;;
    *)
        echo "build-image: unknown target '$target' (known: $targets_all)" >&2
        exit 64
        ;;
    esac

    image="a20os-buildenv:$target"
    echo "build-image: building $image"
    docker build --target "$target" -f "$repo/tools/ci/Dockerfile" \
        -t "$image" "$repo/tools/ci"

    echo "build-image: self-testing $target"
    docker run --rm -v "$repo:/src" -w /src "$image" \
        sh tools/ci/image-selftest.sh "$target"

    if [ "$target" = multi ]; then
        docker tag "$image" a20os-buildenv:latest
        echo "build-image: tagged a20os-buildenv:latest -> multi"
    fi
    echo "build-image: $target OK"
done
