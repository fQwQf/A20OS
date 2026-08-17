#ifndef _ABI_BITS_IOCTLS_H
#define _ABI_BITS_IOCTLS_H

/* A20 Native ABI ioctl numbers, matching the kernel's Linux wire format
 * (kernel/include/core/ioctl.h). */
#define TCGETS        0x5401
#define TCSETS        0x5402
#define TCSETSW       0x5403
#define TCSETSF       0x5404
#define TCSBRK        0x5405
#define TCXONC        0x5406
#define TCSETAW       0x5407
#define TCSETSW2      0x5408
#define TCFLSH        0x540B
#define TIOCEXCL      0x540C
#define TIOCNXCL      0x540D
#define TIOCSCTTY     0x540E
#define TIOCGPGRP     0x540F
#define TIOCSPGRP     0x5410
#define TIOCOUTQ      0x5411
#define TIOCSTI       0x5412
#define TIOCGWINSZ    0x5413
#define TIOCSWINSZ    0x5414
#define TIOCMGET      0x5415
#define TIOCMBIS      0x5416
#define TIOCMBIC      0x5417
#define TIOCMSET      0x5418
#define FIONREAD      0x541B
#define TIOCGPTN      0x80045430
#define FIONBIO       0x5421
#define TIOCSPTLCK    0x40045431
#define FIONCLEX      0x5450
#define FIOCLEX       0x5451

#endif /* _ABI_BITS_IOCTLS_H */
