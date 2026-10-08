#ifndef _ARCH_FCNTL_H
#define _ARCH_FCNTL_H

/* Linux asm-generic fcntl flag values. Architectures with different UAPI
 * assignments provide kernel/arch/<arch>/include/arch/fcntl.h overrides. */
#define ARCH_O_DIRECTORY  0x10000
#define ARCH_O_NOFOLLOW   0x20000
#define ARCH_O_DIRECT     0x4000
#define ARCH_O_LARGEFILE  0x8000

#endif /* _ARCH_FCNTL_H */
