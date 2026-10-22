#!/bin/sh
# Run the IDL malformed-ACK matrix on a reserved architecture/ABI.
#
# Builds the explicit adversarial peer (never part of a production image),
# composes the ordinary dev image, injects the two test binaries, and runs
# tools/run-idl-ack-guest.py, which boots QEMU itself and verifies the exact
# wire-format rejects.  Output goes under build/, which is not tracked.
#
#   tools/run-idl-ack-matrix.sh [aarch64|riscv64]
set -eu

root=$(git rev-parse --show-toplevel)
common=$(git rev-parse --path-format=absolute --git-common-dir)
arch=${1:-aarch64}
case "$arch" in
    aarch64) kernel_name=kernel.bin ;;
    riscv64) kernel_name=kernel.elf ;;
    *) echo "usage: $0 [aarch64|riscv64]" >&2; exit 2 ;;
esac
if [ "$arch" = riscv64 ]; then
    ncpu=NR_CPUS=2
    idl_ack_target=native-rust-idl-ack-test
else
    ncpu=
    idl_ack_target=native-rust-idl-ack-test-$arch
fi
udir="$root/build/idl-ack-user-$arch"
out="$root/build/idl-ack-$arch"
run_build="$common/parallel-rewrite/run-build.sh"

# The ACK binaries are written to a private user build directory so the
# ordinary image build's generated-file cleanup cannot remove them.  The dev
# image is composed first: its user stamp owns (and may clean) that directory,
# and the test-only binaries are injected after composition.
# shellcheck disable=SC2086
sh "$run_build" build-1 make ARCH=$arch ABI=both BRINGUP=0 \
    NATIVE_RUST_PEER=1 $ncpu USER_BUILD_DIR="$udir" dev-build
# shellcheck disable=SC2086
sh "$run_build" build-1 make ARCH=$arch ABI=both BRINGUP=0 \
    NATIVE_RUST_PEER=1 $ncpu USER_BUILD_DIR="$udir" "$idl_ack_target"

bdir=$(make --no-print-directory ARCH=$arch ABI=both BRINGUP=0 $ncpu print-build-dir)
img="$root/$bdir/fat32.img"
kernel="$root/$bdir/$kernel_name"
tag=$arch
[ "$arch" = riscv64 ] && tag=rv

# Inject after image composition: dev-build's native stamp cleanup owns the
# user build directory and would otherwise race these test-only outputs.
mcopy -o -i "$img" "$udir/rust-idl-peer-ack-$tag" "::/rust-idl-peer-ack-$tag"
mcopy -o -i "$img" "$udir/rust-idl-adversary-$tag" "::/rust-idl-adversary-$tag"

mkdir -p "$out"
python3 "$root/tools/run-idl-ack-guest.py" --arch "$arch" \
    --kernel "$kernel" --image "$img" --output "$out" --timeout 180
