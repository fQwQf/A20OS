#!/bin/sh
# Host unit tests for the NAT rewrite half of netfilter.
#
# Compiles tools/host-tests/test-nat-rewrite-host.c together with the real
# kernel/net/filter/netfilter_rewrite.c, so a passing run exercises the shipped source
# rather than a copy of it.  The test supplies its own net_backend_interface_ipv4(),
# which is the only kernel symbol that file needs; everything else in it is
# pure.
#
# Kept out of `make dev-build` on purpose, exactly like
# tools/host-tests/test-tcp-cubic-host.sh: it needs a host cc, and the kernel build must
# not depend on one. Run it directly, or via `make test-nat-rewrite`. Pass
# `rust` to build and exercise the Rust implementation through the same C ABI.
set -e

cd "$(dirname "$0")/../.."
impl=${1:-c}
case "$impl" in
    c|rust) ;;
    *) echo "usage: $0 [c|rust]" >&2; exit 2 ;;
esac

# Manual runs bypass make, which is what pins TMPDIR for every recipe; pin it
# here too so the scratch directory lands under build/ and `make clean` can
# reach it even if this script is killed before its trap fires.
mkdir -p build/tmp
TMPDIR="$PWD/build/tmp"
export TMPDIR

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

objects=
cppflags=
if [ "$impl" = rust ]; then
    rustc=${RUSTC:-rustc}
    host_target=$("$rustc" -vV | sed -n 's/^host: //p')
    extra_flags=
    case "$host_target" in
        x86_64*) extra_flags="-C no-redzone=yes" ;;
    esac
    "$rustc" --edition=2021 --crate-type lib --emit=obj \
        --target "$host_target" -C panic=abort $extra_flags \
        -C opt-level=1 -C codegen-units=1 -C relocation-model=static \
        kernel/net/filter/netfilter_rewrite.rs -o "$OUT/netfilter_rewrite.o"
    objects="$OUT/netfilter_rewrite.o"
    cppflags=-DCONFIG_NETFILTER_REWRITE_RUST
fi

# -Ikernel/include only: netfilter_rewrite.h pulls in core/types.h, which is
# built on compiler-provided integer types, so nothing here drags a freestanding
# kernel <stdint.h> shim over the host one.
${CC:-cc} -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    $cppflags -Ikernel/include \
    -o "$OUT/test-nat-rewrite" \
    tools/host-tests/test-nat-rewrite-host.c \
    kernel/net/filter/netfilter_rewrite.c $objects

echo "NAT_REWRITE_HOST: impl=$impl"
"$OUT/test-nat-rewrite"
