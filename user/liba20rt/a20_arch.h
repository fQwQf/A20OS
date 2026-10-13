/* Native program suffixes shared by self-spawning tests and services. */
#ifndef _A20_ARCH_H
#define _A20_ARCH_H

#if defined(__aarch64__)
#define A20_NATIVE_TAG "aarch64"
#elif defined(__x86_64__)
#define A20_NATIVE_TAG "x86_64"
#elif defined(__loongarch64)
#define A20_NATIVE_TAG "la"
#elif defined(__loongarch__)
#define A20_NATIVE_TAG "la32"
#elif defined(__riscv) && __riscv_xlen == 64
#define A20_NATIVE_TAG "rv"
#elif defined(__riscv)
#define A20_NATIVE_TAG "rv32"
#elif defined(__ARM_ARCH_7M__)
#define A20_NATIVE_TAG "armv7m"
#elif defined(__arm__)
#define A20_NATIVE_TAG "arm32"
#elif defined(__powerpc64__)
#define A20_NATIVE_TAG "ppc64le"
#else
#error "Native program suffix is not defined for this architecture"
#endif

#define A20_NATIVE_BIN(name) "/bin/" name "-" A20_NATIVE_TAG

#endif
