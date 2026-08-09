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
#define TCSBRK        0x5405
#define TCXONC        0x5406
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
#define TIOCGSOFTCAR  0x5419
#define TIOCSSOFTCAR  0x541A
#define FIONREAD      0x541B
#define TIOCINQ       FIONREAD
#define TIOCLINUX     0x541C
#define TIOCCONS      0x541D
#define TIOCGSERIAL   0x541E
#define TIOCSSERIAL   0x541F
#define TIOCPKT       0x5420
#define FIONBIO       0x5421
#define TIOCNOTTY     0x5422
#define TIOCSETD      0x5423
#define TIOCGETD      0x5424
#define TIOCSBRK      0x5427
#define TIOCCBRK      0x5428
#define TIOCGSID      0x5429
#define TIOCGPTN      0x80045430
#define TIOCSPTLCK    0x40045431
#define TIOCGDEV      0x80045432
#define TCGETX        0x5432
#define TCSETX        0x5433
#define TCSETXF       0x5434
#define TCSETXW       0x5435
#define TIOCSIG       0x40045436
#define TIOCGPTPEER   0x5441
#define FIOCLEX       0x5451
#define FIONCLEX      0x5450
#define TIOCGICOUNT   0x545D

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
