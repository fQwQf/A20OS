#!/bin/bash
# Per-file compile check for the net-completion work units.
#
# Uses the exact flags the real kernel build uses (notably -Werror -Wall
# -Wextra -fsanitize=undefined) but stops at -fsyntax-only and writes nothing
# into .kernel-build, so several units can check their own files concurrently
# without racing on the shared object directory.
#
# Usage: tools/net-check.sh kernel/net/socket_control.c [more.c ...]
set -uo pipefail
cd "$(dirname "$0")/.."

status=0
for src in "$@"; do
    [ -f "$src" ] || { echo "MISSING: $src"; status=1; continue; }
    if out=$(/usr/bin/ccache riscv64-unknown-elf-gcc \
            -Wall -Wextra -O3 -ffreestanding -nostdlib -fno-builtin -fno-common \
            -std=gnu99 \
            -Ikernel/arch/riscv64/include -Ikernel/include -Ikernel \
            -Ikernel/net/lwip_port -Ikernel/external/lwip/src/include \
            -Ikernel/platform/qemu-virt-riscv64 \
            -I.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/generated \
            -march=rv64imafdc_zicsr_zifencei -mabi=lp64 -mcmodel=medany \
            -DRISCV64 -DCONFIG_RISCV64 -DCONFIG_ABI_LINUX -DCONFIG_NR_CPUS=1 \
            -DCONFIG_BOARD_QEMU_VIRT_RISCV64 -Werror \
            -fstack-protector-strong -DCONFIG_STACK_PROTECTOR=1 \
            -fsanitize=undefined -fno-sanitize=alignment,bounds-strict \
            -DCONFIG_UBSAN=1 -DCONFIG_DRIVER_DEPLOYMENT_GENERIC \
            -DCONFIG_PCI_MMIO_ALLOC -DCONFIG_PCI_MMIO_BASE_ECAM \
            -DARCH_ELF_MACHINE=243 -DARCH_ELF_CLASS=2 -DCONFIG_64BIT \
            -DARCH_HAS_PGTABLE_OPS -DCONFIG_SWAP -DCONFIG_IOMMU_TRRESP_LEGACY_PPN \
            -fsyntax-only "$src" 2>&1); then
        echo "OK: $src"
    else
        echo "FAIL: $src"
        echo "$out"
        status=1
    fi
done
exit $status
