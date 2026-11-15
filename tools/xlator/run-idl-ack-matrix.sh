#!/bin/sh
# Run the IDL malformed-ACK matrix on a reserved architecture/ABI.
#
# Builds the explicit adversarial peer (never part of a production image),
# composes the ordinary dev image, injects the two test binaries, and runs
# tools/xlator/run-idl-ack-guest.py, which boots QEMU itself and verifies the exact
# wire-format rejects.  Output goes under build/, which is not tracked.
#
#   tools/xlator/run-idl-ack-matrix.sh [aarch64|riscv64]
set -eu

root=$(git rev-parse --show-toplevel)
common=$(git rev-parse --path-format=absolute --git-common-dir)
cd "$root"
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
udir=${IDL_ACK_USER_BUILD_DIR:-"$root/build/idl-ack-user-$arch"}
out=${IDL_ACK_OUTPUT_DIR:-"$root/build/idl-ack-$arch"}
lockdir="$common/parallel-rewrite"
mkdir -p "$lockdir" "$out"
# Use the same leases as the other guest checkers without depending on an
# untracked script inside .git. Keep composition and the private image copy
# under one build lease so no other builder can replace the source image.
exec 9>"$lockdir/activity.lock"
flock -s 9
exec 8>"$lockdir/build-1.lock"
flock -E 75 -n 8

# The ACK binaries are written to a private user build directory so the
# ordinary image build's generated-file cleanup cannot remove them.  The dev
# image is composed first: its user stamp owns (and may clean) that directory,
# and the test-only binaries are injected after composition.
# shellcheck disable=SC2086
make ARCH=$arch ABI=both BRINGUP=0 CONFIG_DEBUG_SHUTDOWN_AUDIT=1 \
    NATIVE_RUST_PEER=1 $ncpu USER_BUILD_DIR="$udir" dev-build
# shellcheck disable=SC2086
make ARCH=$arch ABI=both BRINGUP=0 CONFIG_DEBUG_SHUTDOWN_AUDIT=1 \
    NATIVE_RUST_PEER=1 $ncpu USER_BUILD_DIR="$udir" "$idl_ack_target"

bdir=$(make --no-print-directory ARCH=$arch ABI=both BRINGUP=0 CONFIG_DEBUG_SHUTDOWN_AUDIT=1 $ncpu print-build-dir)
img="$out/fat32.img"
kernel="$root/$bdir/$kernel_name"
tag=$arch
[ "$arch" = riscv64 ] && tag=rv
cp "$root/$bdir/fat32.img" "$img"

# Inject after image composition: dev-build's native stamp cleanup owns the
# user build directory and would otherwise race these test-only outputs.
mcopy -o -i "$img" "$udir/rust-idl-peer-ack-$tag" "::/rust-idl-peer-ack-$tag"
mcopy -o -i "$img" "$udir/rust-idl-adversary-$tag" "::/rust-idl-adversary-$tag"

# The runner boots QEMU itself, so hold the same repository-wide activity (SH)
# + qemu-1 (EX) leases as tools/smoke.py.  A busy slot exits 75, matching the
# other guest checkers. The prepared image is private, so release build-1.
flock -u 8
exec 7>"$lockdir/qemu-1.lock"
flock -E 75 -n 7
python3 "$root/tools/xlator/run-idl-ack-guest.py" --arch "$arch" \
    --kernel "$kernel" --image "$img" --output "$out" --timeout 180
