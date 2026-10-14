#ifdef CONFIG_ARM32

#include "core/string.h"

/* Rust's armv7a codegen may lower aggregate copies/initialization to the ARM
 * EABI memory helpers. libgcc does not provide these memory routines, so
 * provide the narrow ABI bridge used by both MMU and NOMMU kernels. */
void __aeabi_memcpy(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
void __aeabi_memcpy4(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
void __aeabi_memcpy8(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
void __aeabi_memclr(void *dst, size_t n) { memset(dst, 0, n); }
void __aeabi_memclr4(void *dst, size_t n) { memset(dst, 0, n); }
void __aeabi_memclr8(void *dst, size_t n) { memset(dst, 0, n); }

int raise(int sig) {
    (void)sig;
    return 0;
}

#endif
