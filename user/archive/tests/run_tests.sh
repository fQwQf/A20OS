#!/bin/bash
# run_tests.sh — Automated test runner for A20 musl-port.
#
# Mode 1 (host): gcc -DTEST_HOST → run directly on build host
# Mode 2 (QEMU): cross-compile → run on A20 QEMU
#
# Usage:
#   ./run_tests.sh --host          Run host-mode tests (no QEMU needed)
#   ./run_tests.sh --qemu [KERNEL] Run QEMU-mode tests with kernel image
#   ./run_tests.sh --all           Run both modes

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/.build_tests"
mkdir -p "${BUILD_DIR}"

HOST_CC="${CC:-gcc}"
HOST_CFLAGS="-Wall -Wextra -DTEST_HOST"

CROSS="${CROSS_COMPILE:-riscv64-unknown-elf-}"
A20_CC="${CROSS}gcc"
A20_CFLAGS="-O2 -ffreestanding -nostdlib -march=rv64gc -mabi=lp64d -mcmodel=medany -static"
A20_LD="${SCRIPT_DIR}/../a20.ld"

QEMU_CMD="${QEMU:-qemu-system-riscv64}"
KERNEL_IMG=""
TOTAL_PASS=0
TOTAL_FAIL=0

usage() {
    echo "Usage: $0 --host | --qemu KERNEL | --all"
    echo ""
    echo "  --host          Run tests on build host (gcc -DTEST_HOST)"
    echo "  --qemu KERNEL   Run tests under QEMU with KERNEL image"
    echo "  --all           Run both host and QEMU tests"
    exit 1
}

run_host_test() {
    local src="$1"
    local name="$(basename "$src" .c)"
    local bin="${BUILD_DIR}/${name}_host"

    echo "  [HOST] Compiling ${name}..."
    if ! ${HOST_CC} ${HOST_CFLAGS} "$src" -o "$bin" 2>/dev/null; then
        echo "  [HOST] COMPILE FAIL: ${name}"
        return 1
    fi

    echo "  [HOST] Running ${name}..."
    if "$bin"; then
        TOTAL_PASS=$((TOTAL_PASS + 1))
        echo "  [HOST] PASS: ${name}"
    else
        TOTAL_FAIL=$((TOTAL_FAIL + 1))
        echo "  [HOST] FAIL: ${name}"
    fi
}

run_qemu_test() {
    local src="$1"
    local name="$(basename "$src" .c)"
    local bin="${BUILD_DIR}/${name}_a20"

    echo "  [QEMU] Compiling ${name}..."
    if ! ${A20_CC} ${A20_CFLAGS} -T "${A20_LD}" "$src" \
         -o "$bin" 2>/dev/null; then
        echo "  [QEMU] COMPILE FAIL: ${name} (cross-compiler may not be available)"
        return 1
    fi

    echo "  [QEMU] Running ${name} on A20..."
    if [ -z "${KERNEL_IMG}" ]; then
        echo "  [QEMU] SKIP: No kernel image specified"
        return 0
    fi

    # QEMU invocation: boot kernel → task_spawn test binary → capture output
    local output=$(${QEMU_CMD} -machine virt -nographic \
                   -kernel "${KERNEL_IMG}" \
                   -initrd "$bin" \
                   -m 128M \
                   -bios none \
                   2>&1 | head -100)

    if echo "$output" | grep -q "Results: .*0 failed"; then
        TOTAL_PASS=$((TOTAL_PASS + 1))
        echo "  [QEMU] PASS: ${name}"
    else
        TOTAL_FAIL=$((TOTAL_FAIL + 1))
        echo "  [QEMU] FAIL: ${name}"
        echo "$output" | tail -5
    fi
}

run_host_tests() {
    echo ""
    echo "=== Host-mode tests ==="
    echo ""

    for test_src in "${SCRIPT_DIR}"/test_*.c; do
        [ -f "$test_src" ] || continue
        run_host_test "$test_src" || true
    done
}

run_qemu_tests() {
    echo ""
    echo "=== QEMU-mode tests ==="
    echo ""

    for test_src in "${SCRIPT_DIR}"/test_*.c; do
        [ -f "$test_src" ] || continue
        run_qemu_test "$test_src" || true
    done
}

# ---- Main ----
case "${1:-}" in
    --host)
        run_host_tests
        ;;
    --qemu)
        KERNEL_IMG="${2:-}"
        if [ -z "$KERNEL_IMG" ]; then
            echo "ERROR: --qemu requires kernel image path"
            usage
        fi
        run_qemu_tests
        ;;
    --all)
        run_host_tests
        KERNEL_IMG="${2:-}"
        if [ -n "$KERNEL_IMG" ] && [ -f "$KERNEL_IMG" ]; then
            run_qemu_tests
        else
            echo "  SKIP: QEMU tests (no kernel image)"
        fi
        ;;
    *)
        usage
        ;;
esac

echo ""
echo "=== Total: ${TOTAL_PASS} passed, ${TOTAL_FAIL} failed ==="
exit $TOTAL_FAIL
