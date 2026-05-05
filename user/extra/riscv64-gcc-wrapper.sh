#!/bin/bash
# Cross-compiler wrapper that adds musl sysroot paths for riscv64
_REAL_CC=riscv64-unknown-elf-gcc
_EXTRA_FLAGS="-march=rv64g -mabi=lp64 -B@SYSROOT@/lib/ -isystem @CXX_INC@ -isystem @MUSL_BUILD@/obj/include -isystem @MUSL_SRC@/include -isystem @MUSL_SRC@/arch/riscv64 -isystem @MUSL_SRC@/arch/generic -Wno-error=implicit-function-declaration -static -O2 -D_GNU_SOURCE"
case "$0" in
    *g++*|*c++*) _REAL_CC=riscv64-unknown-elf-g++ ; _EXTRA_FLAGS="$_EXTRA_FLAGS -nostdlib++" ;;
esac
exec $_REAL_CC $_EXTRA_FLAGS "$@"
