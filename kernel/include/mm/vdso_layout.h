/*
 * RISC-V64 vDSO fixed user-VA layout.
 *
 * Keep this header preprocessor-only: it is shared by kernel C and the
 * user-space vDSO assembly image.  The vDSO area sits below the lowest
 * address reachable by an 8 MiB user stack.  One page is reserved between
 * the vvar page and the stack limit so stack growth can never consume a
 * vDSO mapping.
 */
#ifndef _MM_VDSO_LAYOUT_H
#define _MM_VDSO_LAYOUT_H

#define A20_VDSO_MAX_PAGES 4
#define A20_VDSO_VA        0x3F7F9000
#define A20_VVAR_VA        0x3F7FE000

#endif
