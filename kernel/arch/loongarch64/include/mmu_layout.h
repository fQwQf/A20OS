#ifndef _ARCH_LOONGARCH64_MMU_LAYOUT_H
#define _ARCH_LOONGARCH64_MMU_LAYOUT_H

/*
 * Keep the hardware page-walk configuration and the software page-table
 * shape in one place.  LA264 boards use the standard four-level layout used
 * by 2K1000LA ports; the QEMU virt target retains its existing folded Dir2
 * layout so its established kernel and userspace path is unchanged.
 */
#ifdef CONFIG_BOARD_LS2K1000
#define LA64_PT_LEVELS       4
#define LA64_PT_ROOT_LEVEL   3
#define LA64_PT_HAS_DIR2     1
#define LA64_PWCL_VALUE      ((12 << 0) | (9 << 5) | \
                              (21 << 10) | (9 << 15) | \
                              (30 << 20) | (9 << 25))
#define LA64_PWCH_VALUE      ((39 << 0) | (9 << 6))
#else
#define LA64_PT_LEVELS       3
#define LA64_PT_ROOT_LEVEL   2
#define LA64_PT_HAS_DIR2     0
#define LA64_PWCL_VALUE      ((12 << 0) | (9 << 5) | \
                              (21 << 10) | (9 << 15) | \
                              (30 << 20) | (9 << 25))
#define LA64_PWCH_VALUE      0
#endif

#endif
