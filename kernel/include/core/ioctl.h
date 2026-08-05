#ifndef _CORE_IOCTL_H
#define _CORE_IOCTL_H

/*
 * Kernel-internal ioctl.h constants.
 * Values match the Linux ABI wire format; abi/linux re-exports them.
 */
#define TCGETS        0x5401
#define TCSETS        0x5402
#define TCSETSW       0x5403
#define TCSETSF       0x5404
#define TIOCGPGRP     0x540F
#define TIOCSPGRP     0x5410
#define TIOCGWINSZ    0x5413
#define TIOCSWINSZ    0x5414
#define TIOCSCTTY     0x540E
#define TIOCGPTN      0x80045430
#define TIOCSPTLCK    0x40045431
#define FIONBIO       0x5421
#define FIONREAD      0x541B

#define PPC64_TCGETS       0x402C7413UL
#define PPC64_TCSETS       0x802C7414UL
#define PPC64_TCSETSW      0x802C7415UL
#define PPC64_TCSETSF      0x802C7416UL
#define PPC64_TIOCSPGRP    0x80047476UL
#define PPC64_TIOCGPGRP    0x40047477UL
#define PPC64_TIOCSWINSZ   0x80087467UL
#define PPC64_TIOCGWINSZ   0x40087468UL
#define PPC64_FIONBIO      0x8004667EUL
#define PPC64_FIONREAD     0x4004667FUL

#endif
