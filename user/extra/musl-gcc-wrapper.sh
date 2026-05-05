#!/bin/bash
# Wrapper for riscv64-unknown-elf-gcc that injects musl CRT/libc
# in the correct link positions for cross-compilation with musl.
#
# Compile-only flags (-c, -S, -E): pass through with musl CFLAGS
# Link commands: crt1.o crti.o [objects] libc.a libgcc.a crtn.o

MUSL_BUILD="@MUSL_BUILD@"
LIBGCC="@LIBGCC@"
MUSL_CRT="$MUSL_BUILD/lib"
EXTRA_CFLAGS="@CFLAGS@"
REAL_CC="@REAL_CC@"

CRT_START="$MUSL_CRT/crt1.o $MUSL_CRT/crti.o"
CRT_END="$MUSL_CRT/crtn.o"
LIBC="$MUSL_CRT/libc.a $LIBGCC"

compile_only=false
for arg in "$@"; do
    case "$arg" in
        -c|-S|-E|-M|-MM|-MG|-MP) compile_only=true; break ;;
    esac
done

if $compile_only; then
    exec $REAL_CC $EXTRA_CFLAGS "$@"
fi

has_output=false
for arg in "$@"; do
    case "$arg" in
        -o) has_output=true; break ;;
    esac
done

if ! $has_output; then
    exec $REAL_CC $EXTRA_CFLAGS -static -nostdlib $CRT_START "$@" $LIBC $CRT_END
fi

exec $REAL_CC $EXTRA_CFLAGS -static -nostdlib $CRT_START "$@" $LIBC $CRT_END
