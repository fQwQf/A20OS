#!/bin/bash
set -euo pipefail

# Build a minimal GCC toolchain (riscv64 native) using musl-cross-make
# as the cross-compiler.  Binaries are dynamically linked against musl;
# the dynamic linker (libc.so) ships on the extra disk image.
#
# Usage: build-gcc.sh <arch> <musl_build_dir> <build_dir> <install_dir>

ARCH="${1:?Usage: $0 <arch> <musl_build_dir> <build_dir> <install_dir>}"
mkdir -p "$2" "$3" "$4"
MUSL_BUILD="$(cd "$2" && pwd)"
BUILD_DIR="$(cd "$3" && pwd)"
INSTALL_DIR="$(cd "$4" && pwd)"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
USER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MCM="$USER_DIR/external/musl-cross-make/output"
BINUTILS_SRC="$USER_DIR/external/binutils"
GCC_SRC="$USER_DIR/external/gcc"

BUILD_TRIPLET="aarch64-pc-linux-gnu"
HOST_TRIPLET="riscv64-linux-musl"
TARGET_TRIPLET="riscv64-linux-musl"

MCM_CC="$MCM/bin/${HOST_TRIPLET}-gcc"
MCM_CXX="$MCM/bin/${HOST_TRIPLET}-g++"
MCM_AR="$MCM/bin/${HOST_TRIPLET}-ar"
MCM_RANLIB="$MCM/bin/${HOST_TRIPLET}-ranlib"
MCM_STRIP="$MCM/bin/${HOST_TRIPLET}-strip"
MCM_SYSROOT="$MCM/bin/../$HOST_TRIPLET"

echo "=== build-gcc.sh ==="
echo "  BUILD=$BUILD_TRIPLET  HOST=$HOST_TRIPLET  TARGET=$TARGET_TRIPLET"
echo "  MCM_CC=$MCM_CC"
echo ""

export PATH="$MCM/bin:$PATH"

$MCM_CC --version | head -1
echo 'int main(){return 0;}' | $MCM_CC -x c - -o /dev/null 2>&1 \
    && echo "Cross-compiler test: OK" || { echo "Cross-compiler test: FAILED"; exit 1; }

# ============================================================
# Step 1: Build binutils (riscv64 host, riscv64 target)
# ============================================================
BINUTILS_BUILD="$BUILD_DIR/binutils"
BINUTILS_INSTALL="$BUILD_DIR/binutils-install"

if [ ! -f "$BINUTILS_INSTALL/usr/bin/as" ]; then
    echo "[GCC] Step 1/3: Building binutils..."
    rm -rf "$BINUTILS_BUILD"
    mkdir -p "$BINUTILS_BUILD"
    cd "$BINUTILS_BUILD"

    "$BINUTILS_SRC/configure" \
        --build="$BUILD_TRIPLET" \
        --host="$HOST_TRIPLET" \
        --target="$TARGET_TRIPLET" \
        --prefix=/usr \
        --disable-nls \
        --disable-werror \
        --disable-gdb \
        --disable-sim \
        --disable-gold \
        --disable-gprofng \
        --with-sysroot=/ \
        CC="$MCM_CC" \
        CXX="$MCM_CXX" \
        CC_FOR_BUILD=gcc \
        CXX_FOR_BUILD=g++ \
        CFLAGS="-O2" \
        AR="$MCM_AR" \
        RANLIB="$MCM_RANLIB" \
        2>&1 | tail -10

    echo "[GCC] Compiling binutils..."
    make -j$(nproc) 2>&1 | tail -10

    echo "[GCC] Installing binutils..."
    make DESTDIR="$BINUTILS_INSTALL" install 2>&1 | tail -5

    # Strip the installed binaries
    for f in "$BINUTILS_INSTALL"/usr/bin/*; do
        [ -f "$f" ] && $MCM_STRIP "$f" 2>/dev/null || true
    done
    echo "[GCC] binutils done."
else
    echo "[GCC] binutils already built, skipping."
fi

# ============================================================
# Step 2: Build GCC — C only, minimal (riscv64 host, riscv64 target)
# ============================================================
GCC_BUILD="$BUILD_DIR/gcc-build"
GCC_INSTALL="$BUILD_DIR/gcc-install"

if [ ! -f "$GCC_INSTALL/usr/bin/gcc" ]; then
    echo "[GCC] Step 2/3: Building GCC (C only, minimal)..."
    rm -rf "$GCC_BUILD"
    mkdir -p "$GCC_BUILD"
    cd "$GCC_BUILD"

    "$GCC_SRC/configure" \
        --build="$BUILD_TRIPLET" \
        --host="$HOST_TRIPLET" \
        --target="$TARGET_TRIPLET" \
        --prefix=/usr \
        --enable-languages=c \
        --disable-bootstrap \
        --disable-multilib \
        --disable-nls \
        --disable-shared \
        --disable-threads \
        --disable-tls \
        --disable-libssp \
        --disable-libgomp \
        --disable-libmudflap \
        --disable-libquadmath \
        --disable-libstdcxx \
        --disable-libitm \
        --disable-libatomic \
        --disable-libasan \
        --disable-libtsan \
        --disable-liblsan \
        --disable-libubsan \
        --without-isl \
        --without-zstd \
        --with-sysroot=/ \
        --with-build-sysroot="$MCM_SYSROOT" \
        CC="$MCM_CC" \
        CXX="$MCM_CXX" \
        CC_FOR_BUILD=gcc \
        CXX_FOR_BUILD=g++ \
        CFLAGS="-O2" \
        CXXFLAGS="-O2" \
        AR="$MCM_AR" \
        RANLIB="$MCM_RANLIB" \
        2>&1 | tail -20

    echo "[GCC] Disabling libcody..."
    if [ -d "$GCC_BUILD/libcody" ]; then
        rm -rf "$GCC_BUILD/libcody"
        mkdir -p "$GCC_BUILD/libcody"
        echo 'all install clean mostlyclean distclean maintainer-clean check install-strip:' > "$GCC_BUILD/libcody/Makefile"
    fi

    echo "[GCC] Compiling GCC (all-gcc only, -j$(nproc))..."
    make -j$(nproc) all-gcc 2>&1 | tail -10

    echo "[GCC] Installing GCC (all-gcc only)..."
    make DESTDIR="$GCC_INSTALL" install-gcc 2>&1 | tail -10

    # Strip
    for f in "$GCC_INSTALL"/usr/bin/*; do
        [ -f "$f" ] && $MCM_STRIP "$f" 2>/dev/null || true
    done
    for f in "$GCC_INSTALL"/usr/libexec/gcc/$TARGET_TRIPLET/*/cc1 \
             "$GCC_INSTALL"/usr/libexec/gcc/$TARGET_TRIPLET/*/collect2 \
             "$GCC_INSTALL"/usr/libexec/gcc/$TARGET_TRIPLET/*/cpp; do
        [ -f "$f" ] && $MCM_STRIP "$f" 2>/dev/null || true
    done
    echo "[GCC] GCC done."
else
    echo "[GCC] GCC already built, skipping."
fi

# ============================================================
# Step 3: Package into INSTALL_DIR (preserving versioned paths)
# ============================================================
echo "[GCC] Step 3/3: Packaging..."

GCC_VER=$(ls "$GCC_INSTALL/usr/libexec/gcc/$TARGET_TRIPLET/" 2>/dev/null | head -1)
echo "[GCC] GCC version: $GCC_VER"

mkdir -p "$INSTALL_DIR/bin"
mkdir -p "$INSTALL_DIR/libexec/gcc/$TARGET_TRIPLET/$GCC_VER"
mkdir -p "$INSTALL_DIR/lib/gcc/$TARGET_TRIPLET/$GCC_VER"

for tool in as ld ar nm ranlib objcopy objdump readelf strip strings addr2line; do
    for cand in "$BINUTILS_INSTALL/usr/bin/$TARGET_TRIPLET-$tool" \
                "$BINUTILS_INSTALL/usr/bin/$tool" \
                "$BINUTILS_INSTALL/bin/$tool"; do
        if [ -f "$cand" ]; then
            cp "$cand" "$INSTALL_DIR/bin/$tool"
            break
        fi
    done
done

for cand in "$GCC_INSTALL/usr/bin/$TARGET_TRIPLET-gcc" "$GCC_INSTALL/usr/bin/gcc" "$GCC_INSTALL/bin/gcc"; do
    if [ -f "$cand" ]; then
        cp "$cand" "$INSTALL_DIR/bin/gcc"
        break
    fi
done

for f in cc1 collect2; do
    src="$GCC_INSTALL/usr/libexec/gcc/$TARGET_TRIPLET/$GCC_VER/$f"
    [ -f "$src" ] && cp "$src" "$INSTALL_DIR/libexec/gcc/$TARGET_TRIPLET/$GCC_VER/$f"
done

for f in specs; do
    src="$GCC_INSTALL/usr/lib/gcc/$TARGET_TRIPLET/$GCC_VER/$f"
    [ -f "$src" ] && cp "$src" "$INSTALL_DIR/lib/gcc/$TARGET_TRIPLET/$GCC_VER/$f"
done

for f in "$MCM"/lib/gcc/$TARGET_TRIPLET/*/libgcc.a \
         "$MCM"/$TARGET_TRIPLET/lib/libgcc.a \
         "$MCM"/lib/gcc/libgcc.a; do
    if [ -f "$f" ]; then cp "$f" "$INSTALL_DIR/lib/gcc/$TARGET_TRIPLET/$GCC_VER/libgcc.a"; break; fi
done

for f in "$MCM"/$TARGET_TRIPLET/lib/libc.a "$MCM"/$TARGET_TRIPLET/lib/libc.so; do
    [ -f "$f" ] && cp "$f" "$INSTALL_DIR/lib/gcc/$TARGET_TRIPLET/$GCC_VER/" && break
done

for crt in crt1.o crti.o crtn.o Scrt1.o rcrt1.o; do
    for d in "$MCM"/$TARGET_TRIPLET/lib "$MCM"/lib; do
        if [ -f "$d/$crt" ]; then
            cp "$d/$crt" "$INSTALL_DIR/lib/gcc/$TARGET_TRIPLET/$GCC_VER/$crt"
            break
        fi
    done
done

ln -sf gcc "$INSTALL_DIR/bin/cc"

echo "[GCC] Done."
echo "=== bin ==="
ls -lh "$INSTALL_DIR/bin/" 2>/dev/null
echo "=== libexec/gcc ==="
ls -lhR "$INSTALL_DIR/libexec/gcc/" 2>/dev/null
echo "=== lib/gcc ==="
ls -lhR "$INSTALL_DIR/lib/gcc/" 2>/dev/null
