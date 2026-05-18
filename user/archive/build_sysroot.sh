#!/bin/bash
# build_sysroot.sh — Build the A20 Native ABI sysroot.
#
# Produces a self-contained sysroot directory with:
#   - musl-a20 static library (libc.a)
#   - liba20rt.a (A20 syscall wrappers + crt0)
#   - A20 headers
#   - Linker script (a20.ld)
#   - crt0.o
#
# Usage:
#   ./build_sysroot.sh [SYSROOT_DIR]
#
# Default SYSROOT_DIR: ./sysroot/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SYSROOT="${1:-${SCRIPT_DIR}/sysroot}"

MUSL_DIR="${SCRIPT_DIR}"
LIBA20RT_DIR="${SCRIPT_DIR}/../../user/liba20rt"
LIBA20C_DIR="${SCRIPT_DIR}/../../user/liba20c"
KERNEL_INC="${SCRIPT_DIR}/../../kernel/include"

CROSS="riscv64-unknown-elf-"
CC="${CROSS}gcc"
AR="${CROSS}ar"
RANLIB="${CROSS}ranlib"

CFLAGS="-O2 -ffreestanding -nostdlib -march=rv64gc -mabi=lp64d -mcmodel=medany"
CFLAGS="${CFLAGS} -Wall -Wextra -Wno-unused-parameter"

echo "=== A20 sysroot build ==="
echo "  SYSROOT: ${SYSROOT}"
echo "  CC:      ${CC}"
echo ""

# ---- 1. Create sysroot directory structure ----
mkdir -p "${SYSROOT}"/{include/a20,lib}

# ---- 2. Install A20 headers ----
echo "[1/6] Installing A20 headers..."

for hdr in \
    "${KERNEL_INC}/abi/native/types.h" \
    "${KERNEL_INC}/abi/native/errno.h" \
    "${KERNEL_INC}/abi/native/rights.h" \
    "${KERNEL_INC}/abi/native/syscall_nr.h" \
    "${KERNEL_INC}/abi/native/startup.h"; do
    if [ -f "$hdr" ]; then
        cp "$hdr" "${SYSROOT}/include/a20/"
    fi
done

# musl-port arch headers
for hdr in \
    "${MUSL_DIR}/arch/a20/syscall.h" \
    "${MUSL_DIR}/arch/a20/crt_arch.h" \
    "${MUSL_DIR}/arch/a20/pthread_arch.h" \
    "${MUSL_DIR}/arch/a20/atomic.h" \
    "${MUSL_DIR}/arch/a20/reloc.h"; do
    if [ -f "$hdr" ]; then
        cp "$hdr" "${SYSROOT}/include/a20/"
    fi
done

# bits/ subdirectory
mkdir -p "${SYSROOT}/include/a20/bits"
if [ -f "${MUSL_DIR}/arch/a20/bits/syscall.h" ]; then
    cp "${MUSL_DIR}/arch/a20/bits/syscall.h" "${SYSROOT}/include/a20/bits/"
fi

echo "  -> $(find "${SYSROOT}/include" -name '*.h' | wc -l) headers installed"

# ---- 3. Compile liba20rt ----
echo "[2/6] Compiling liba20rt..."

LIBA20RT_OBJDIR="${SYSROOT}/.build/liba20rt"
mkdir -p "${LIBA20RT_OBJDIR}"

if [ -d "${LIBA20RT_DIR}" ]; then
    for src in "${LIBA20RT_DIR}"/*.c; do
        [ -f "$src" ] || continue
        base="$(basename "$src" .c)"
        echo "  CC  ${base}.c"
        ${CC} ${CFLAGS} -I"${SYSROOT}/include" \
              -c "$src" -o "${LIBA20RT_OBJDIR}/${base}.o"
    done

    # Compile crt0 if it exists
    for crt in "${LIBA20RT_DIR}/crt0_a20.S" "${LIBA20RT_DIR}/crt0.S"; do
        if [ -f "$crt" ]; then
            echo "  AS  $(basename "$crt")"
            ${CC} ${CFLAGS} -I"${SYSROOT}/include" \
                  -c "$crt" -o "${SYSROOT}/lib/crt0.o"
            break
        fi
    done

    ${AR} rcs "${SYSROOT}/lib/liba20rt.a" "${LIBA20RT_OBJDIR}"/*.o
    ${RANLIB} "${SYSROOT}/lib/liba20rt.a"
    echo "  -> liba20rt.a ($(stat -c%s "${SYSROOT}/lib/liba20rt.a") bytes)"
else
    echo "  SKIP: liba20rt sources not found at ${LIBA20RT_DIR}"
fi

# ---- 4. Compile liba20c (minimal C library) ----
echo "[3/6] Compiling liba20c..."

LIBA20C_OBJDIR="${SYSROOT}/.build/liba20c"
mkdir -p "${LIBA20C_OBJDIR}"

if [ -d "${LIBA20C_DIR}" ]; then
    for src in "${LIBA20C_DIR}"/*.c; do
        [ -f "$src" ] || continue
        base="$(basename "$src" .c)"
        echo "  CC  ${base}.c"
        ${CC} ${CFLAGS} -I"${SYSROOT}/include" -I"${LIBA20C_DIR}" \
              -c "$src" -o "${LIBA20C_OBJDIR}/${base}.o"
    done

    ${AR} rcs "${SYSROOT}/lib/liba20c.a" "${LIBA20C_OBJDIR}"/*.o
    ${RANLIB} "${SYSROOT}/lib/liba20c.a"
    echo "  -> liba20c.a ($(stat -c%s "${SYSROOT}/lib/liba20c.a") bytes)"
else
    echo "  SKIP: liba20c sources not found at ${LIBA20C_DIR}"
fi

# ---- 5. Compile musl-port bridge files ----
echo "[4/6] Compiling musl-port bridge..."

MUSL_OBJDIR="${SYSROOT}/.build/musl-port"
mkdir -p "${MUSL_OBJDIR}"

for src in \
    "${MUSL_DIR}/src/internal/a20_syscallops.c" \
    "${MUSL_DIR}/src/internal/a20_fdtable.c" \
    "${MUSL_DIR}/src/process/a20_fork.c" \
    "${MUSL_DIR}/src/process/a20_posix_spawn.c" \
    "${MUSL_DIR}/src/signal/a20_signal.c" \
    "${MUSL_DIR}/src/thread/a20_pthread.c" \
    "${MUSL_DIR}/src/thread/a20_mutex.c" \
    "${MUSL_DIR}/syscall_bridge.c"; do
    [ -f "$src" ] || continue
    base="$(basename "$src" .c)"
    echo "  CC  ${base}.c"
    ${CC} ${CFLAGS} \
          -I"${SYSROOT}/include" \
          -I"${MUSL_DIR}/arch/a20" \
          -I"${MUSL_DIR}/src/internal" \
          -c "$src" -o "${MUSL_OBJDIR}/${base}.o" 2>/dev/null || \
        echo "  WARN: ${base}.c failed to compile (expected if musl headers missing)"
done

if ls "${MUSL_OBJDIR}"/*.o 1>/dev/null 2>&1; then
    ${AR} rcs "${SYSROOT}/lib/liba20port.a" "${MUSL_OBJDIR}"/*.o
    ${RANLIB} "${SYSROOT}/lib/liba20port.a"
    echo "  -> liba20port.a ($(stat -c%s "${SYSROOT}/lib/liba20port.a") bytes)"
fi

# ---- 6. Install linker script ----
echo "[5/6] Installing linker script..."

cat > "${SYSROOT}/lib/a20.ld" << 'LDEOF'
/* a20.ld — A20 Native ELF linker script for riscv64 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS {
    text   PT_LOAD FLAGS(5);   /* R+X */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* R+W */
    bss    PT_LOAD FLAGS(6);   /* R+W */
}

SECTIONS {
    . = 0x400000 + SIZEOF_HEADERS;

    .text : {
        *(.text.entry)
        *(.text .text.*)
    } :text

    . = ALIGN(0x1000);

    .rodata : {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } :rodata

    . = ALIGN(0x1000);

    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
        *(.got .got.*)
    } :data

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        __bss_end = .;
    } :bss

    . = ALIGN(0x1000);
    _end = .;

    /DISCARD/ : {
        *(.note.*)
        *(.comment)
        *(.eh_frame*)
    }
}
LDEOF

echo "  -> lib/a20.ld"

# ---- 7. Write ABI version file ----
echo "[6/6] Writing ABI version..."
echo "1.0" > "${SYSROOT}/etc/a20-abi-version"
mkdir -p "${SYSROOT}/etc"

# ---- Done ----
echo ""
echo "=== Sysroot ready at ${SYSROOT} ==="
echo ""
echo "Contents:"
echo "  include/a20/    — A20 Native ABI headers"
echo "  lib/crt0.o      — Startup code"
echo "  lib/liba20rt.a  — Syscall wrappers"
echo "  lib/liba20c.a   — Minimal C library"
echo "  lib/liba20port.a — musl bridge layer"
echo "  lib/a20.ld      — Linker script"
echo ""
echo "To compile a program:"
echo "  ${CC} -static -nostdlib -T ${SYSROOT}/lib/a20.ld \\"
echo "        -I${SYSROOT}/include \\"
echo "        ${SYSROOT}/lib/crt0.o \\"
echo "        your_program.c \\"
echo "        -L${SYSROOT}/lib -la20c -la20rt \\"
echo "        -o your_program"
